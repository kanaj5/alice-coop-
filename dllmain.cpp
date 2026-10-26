#include "pch.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

static HMODULE g_real = nullptr;
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;

typedef DWORD(WINAPI* PFN_GetState)(DWORD, void*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, void*);
typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, void*);
typedef void  (WINAPI* PFN_Enable)(BOOL);

static PFN_GetState p_GetState = nullptr;
static PFN_SetState p_SetState = nullptr;
static PFN_GetCaps  p_GetCaps = nullptr;
static PFN_Enable   p_Enable = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args; va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fprintf(g_log, "\n");
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// ---- Funciones que exportamos: solo reenvian a la DLL real ----
extern "C" DWORD WINAPI XInputGetState(DWORD i, void* s) {
    return p_GetState ? p_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD WINAPI XInputSetState(DWORD i, void* v) {
    return p_SetState ? p_SetState(i, v) : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void* c) {
    return p_GetCaps ? p_GetCaps(i, f, c) : ERROR_DEVICE_NOT_CONNECTED;
}
extern "C" void WINAPI XInputEnable(BOOL e) {
    if (p_Enable) p_Enable(e);
}

// ---- Lee un modulo cargado: base, tamano y lista de exports ----
static void DumpModule(const char* name, HMODULE m) {
    BYTE* base = (BYTE*)m;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    Log("%s: base real=%p, base preferida=%08X, tamano=%u bytes", name, base,
        (unsigned)nt->OptionalHeader.ImageBase, (unsigned)nt->OptionalHeader.SizeOfImage);

    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) { Log("%s: no tiene exports", name); return; }

    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    WORD* ords = (WORD*)(base + exp->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
    Log("%s: %u exports en total, %u con nombre", name,
        (unsigned)exp->NumberOfFunctions, (unsigned)exp->NumberOfNames);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        Log("   %s  (RVA %08X, direccion %p)", (char*)(base + names[i]),
            (unsigned)funcs[ords[i]], base + funcs[ords[i]]);
    }
}

static void DumpModuleSafe(const char* name, HMODULE m) {
    __try { DumpModule(name, m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("%s: error al leer el modulo", name); }
}

// ====== HOOKS: parche de la tabla de funciones de fgamex86.dll ======
static const DWORD RVA_TABLA_EXPORTS = 0x001B93F8;
static const DWORD CASILLA_CLIENTCONNECT = 0x14;
static const DWORD CASILLA_CLIENTBEGIN = 0x18;
static const DWORD RVA_CLIENTCONNECT = 0x00075310;
static const DWORD RVA_CLIENTBEGIN = 0x00073880;

typedef const char* (__cdecl* PFN_ClientConnect)(int clientNum, int firstTime);
static PFN_ClientConnect g_origClientConnect = nullptr;
static DWORD g_origClientBegin = 0;

static const char* __cdecl Detour_ClientConnect(int clientNum, int firstTime) {
    Log("[HOOK] ClientConnect(clientNum=%d, firstTime=%d)", clientNum, firstTime);
    const char* r = g_origClientConnect ? g_origClientConnect(clientNum, firstTime) : "error del hook";
    Log("[HOOK] ClientConnect -> %s", r ? r : "NULL (aceptado)");
    return r;
}

static void __cdecl LogClientBegin(DWORD ent) {
    Log("[HOOK] ClientBegin(entidad=%08X)", ent);
}

static __declspec(naked) void Detour_ClientBegin() {
    __asm {
        pushad
        mov  eax, dword ptr[esp + 36]
        push eax
        call LogClientBegin
        add  esp, 4
        popad
        jmp  dword ptr[g_origClientBegin]
    }
}

struct SlotPatch {
    const char* nombre;
    DWORD casilla;
    DWORD rvaEsperada;
    DWORD desvio;
    DWORD* original;
    DWORD ultimoMalo;
};

static SlotPatch g_patches[2] = {
    { "ClientConnect", CASILLA_CLIENTCONNECT, RVA_CLIENTCONNECT, 0, nullptr, 0 },
    { "ClientBegin",   CASILLA_CLIENTBEGIN,   RVA_CLIENTBEGIN,   0, nullptr, 0 },
};

static void ApplyPatches(BYTE* base) {
    for (int i = 0; i < 2; i++) {
        SlotPatch& p = g_patches[i];
        DWORD* slot = (DWORD*)(base + RVA_TABLA_EXPORTS + p.casilla);
        DWORD actual = 0;
        __try { actual = *slot; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        if (actual == 0 || actual == p.desvio) continue;

        DWORD esperado = (DWORD)(base + p.rvaEsperada);
        if (actual != esperado) {
            if (actual != p.ultimoMalo) {
                Log("%s: valor inesperado %08X (esperaba %08X); no se parchea", p.nombre, actual, esperado);
                p.ultimoMalo = actual;
            }
            continue;
        }

        DWORD oldProt = 0;
        if (VirtualProtect(slot, 4, PAGE_READWRITE, &oldProt)) {
            *p.original = actual;
            *slot = p.desvio;
            VirtualProtect(slot, 4, oldProt, &oldProt);
            Log("%s: casilla parcheada (original %08X)", p.nombre, actual);
        }
    }
}

// ---- Diagnostico: vuelca la tabla de funciones del juego ----
static void DumpTable(BYTE* base) {
    __try {
        DWORD* t = (DWORD*)(base + RVA_TABLA_EXPORTS);
        Log("Tabla de exports en %p (version=%u), base del modulo=%p", t, (unsigned)t[0], base);
        for (int i = 1; i <= 25; i += 4)
            Log("  +%02X: %08X %08X %08X %08X", i * 4, t[i], t[i + 1], t[i + 2], t[i + 3]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("No se pudo leer la tabla de exports");
    }
}

// ====== Registro de fallos ======
static void DescribeAddress(DWORD addr, char* out, size_t n) {
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)addr, &hm) && hm) {
        char path[MAX_PATH];
        GetModuleFileNameA(hm, path, MAX_PATH);
        const char* nm = strrchr(path, '\\');
        nm = nm ? nm + 1 : path;
        sprintf_s(out, n, "%s+0x%X", nm, (unsigned)(addr - (DWORD)hm));
    }
    else {
        sprintf_s(out, n, "?");
    }
}

static bool IsCodeAddress(DWORD addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static bool SafeRead(const DWORD* p, DWORD* out) {
    __try { *out = *p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static LONG g_crashLogs = 0;

static LONG CALLBACK CrashLogger(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashLogs) > 20) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* c = ep->ContextRecord;
    char where[160];
    DescribeAddress((DWORD)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    Log("[EXCEPCION] codigo=%08X en %s (EIP=%08X) hilo=%lu", code, where, c->Eip, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR tipo = ep->ExceptionRecord->ExceptionInformation[0];
        Log("[EXCEPCION] %s la direccion %08X",
            tipo == 0 ? "lectura de" : (tipo == 1 ? "escritura en" : "ejecucion de"),
            (unsigned)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    Log("[EXCEPCION] EAX=%08X EBX=%08X ECX=%08X EDX=%08X", c->Eax, c->Ebx, c->Ecx, c->Edx);
    Log("[EXCEPCION] ESI=%08X EDI=%08X EBP=%08X ESP=%08X", c->Esi, c->Edi, c->Ebp, c->Esp);

    DWORD* sp = (DWORD*)c->Esp;
    int hits = 0;
    for (int i = 0; i < 1024 && hits < 40; i++) {
        DWORD v = 0;
        if (!SafeRead(sp + i, &v)) break;
        if (!IsCodeAddress(v)) continue;
        char d[160];
        DescribeAddress(v, d, sizeof(d));
        if (strstr(d, "alice.exe") || strstr(d, "fgamex86") || strstr(d, "cgamex86")) {
            Log("[EXCEPCION]   pila[+%03X] = %s", i * 4, d);
            hits++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- Hilo propio: vigila los modulos del juego y aplica los parches ----
static DWORD WINAPI InitThread(LPVOID) {
    g_patches[0].desvio = (DWORD)&Detour_ClientConnect;
    g_patches[0].original = (DWORD*)&g_origClientConnect;
    g_patches[1].desvio = (DWORD)&Detour_ClientBegin;
    g_patches[1].original = &g_origClientBegin;

    HMODULE exe = GetModuleHandleA(nullptr);
    Log("alice.exe base = %p", (void*)exe);

    const char* names[] = { "cgamex86.dll", "fgamex86.dll" };
    HMODULE last[2] = { nullptr, nullptr };
    DWORD loadTick = 0;
    bool diagDone = true;
    for (;;) {
        for (int i = 0; i < 2; i++) {
            HMODULE m = GetModuleHandleA(names[i]);
            if (m != last[i]) {
                if (m) { Log("%s CARGADO en %p", names[i], (void*)m); DumpModuleSafe(names[i], m); }
                else     Log("%s DESCARGADO", names[i]);
                last[i] = m;
                if (i == 1 && m) { loadTick = GetTickCount(); diagDone = false; }
            }
        }
        if (last[1]) {
            ApplyPatches((BYTE*)last[1]);
            if (!diagDone && GetTickCount() - loadTick > 3000) {
                DumpTable((BYTE*)last[1]);
                diagDone = true;
            }
        }
        Sleep(5);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_logLock);

        char dir[MAX_PATH];
        GetModuleFileNameA(hModule, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\');
        if (slash) *(slash + 1) = '\0';
        char logPath[MAX_PATH];
        sprintf_s(logPath, "%salice-coop_%lu.log", dir, GetCurrentProcessId());
        fopen_s(&g_log, logPath, "w");
        Log("=== alice-coop proxy v0.5 (hooks + registro de fallos) iniciado (PID %lu) ===", GetCurrentProcessId());

        AddVectoredExceptionHandler(1, CrashLogger);

        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat_s(sys, "\\xinput1_3.dll");
        g_real = LoadLibraryA(sys);
        if (g_real) {
            p_GetState = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
            p_SetState = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
            p_GetCaps = (PFN_GetCaps)GetProcAddress(g_real, "XInputGetCapabilities");
            p_Enable = (PFN_Enable)GetProcAddress(g_real, "XInputEnable");
            Log("DLL real cargada: %s", sys);
        }
        else {
            Log("ERROR: no se pudo cargar %s (error %lu)", sys, GetLastError());
        }

        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}