#include "pch.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ===== OPCIONES =====
static const bool PARCHE_RUNENTITY = true;   // omite objetos sin 'edict' en G_RunEntity
static const bool PARCHE_ESTADO = true;   // omite el "estado STAND" cuando la cadena de punteros es invalida

// ===== OPCIONES QUE SE CAMBIAN SIN RECOMPILAR (archivo alice-coop.ini junto al juego) =====
static char g_iniPath[MAX_PATH] = "";
static int  g_optArbolCliente = 1;    // ArreglarArbolCliente: 1 = el cliente puro marca vacio el arbol de sectores
static int  g_optSaltarReset = 1;     // SaltarResetClientes: 1 = ClientBegin de los clientes 1+ no ejecuta FUN_100d2750
static int  g_optWatchHW = 0;     // HardwareWatch: 1 = vigilancia por hardware (ya no hace falta)
static int  g_optHookThink = 1;     // HookClientThink: 1 = cuenta las llamadas de ClientThink de cada cliente
static int  g_optHookDisc = 1;     // HookClientDisconnect: 1 = registra las desconexiones
static int  g_optDiagMs = 1000;  // DiagIntervaloMs: cada cuantos ms se vuelca el estado de los jugadores
static int  g_optEquipo = 0;     // CopiarEquipoCliente1: 1 = copia el campo de equipo (obj+0x34C) del jugador 0 al cliente 1 si difiere
static int  g_optPersist = 1;     // HookPersistencia: registra ReadPersistant/WritePersistant (inventario entre mapas)
static int  g_optRelectura = 1;     // RelecturaPersistente: tras el ClientBegin del cliente 1 en un cambio de mapa, vuelve a leer la persistencia (inventario)
static int  g_optSepararSpawn = 1;     // SepararSpawn: 1 = al entrar el cliente 1, se coloca a SepararDistancia unidades del jugador 0 (evita que queden pegados)
static int  g_optSepararDist = 64;
static int  g_optSeguir = 0;     // SeguirJugador0: 1 = el cliente 1 se teletransporta junto al jugador 0 si se aleja
static int  g_optSeguirDist = 1500;  // SeguirDistancia (unidades)
static int  g_optSeguirMs = 3000;  // SeguirEsperaMs entre teletransportes
static int  g_optCambiosMs = 60000; // CambiosDuracionMs: cuanto tiempo tras entrar el cliente 1 se registran [CAMBIO]
static int  g_optProt0Ms = 5000;  // ProtegerJugador0Ms: modo dios para el jugador 0 desde ANTES del ClientBegin del cliente 1 (evita que el spawn lo mate)
static int  g_optProtegerMs = 30000; // ProtegerClienteMs: modo dios (obj+0x180 bit 8) para el cliente 1 tras ClientBegin; 0 = no
static int  g_optDiagDurMs = 7200000; // DiagDuracionMs: durante cuantos ms tras entrar el cliente 1

static HMODULE g_real = nullptr;
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;
static BYTE* g_fgameBase = nullptr;
static __declspec(thread) int t_silencio = 0;   // >0 = lecturas de prueba (no registrar excepciones)

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

static bool SafeRead(const DWORD* p, DWORD* out) {
    t_silencio++;
    bool ok = true;
    __try { *out = *p; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    t_silencio--;
    return ok;
}

static bool SafeReadByte(DWORD addr) {
    t_silencio++;
    bool ok = true;
    __try { volatile BYTE b = *(volatile BYTE*)addr; (void)b; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    t_silencio--;
    return ok;
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

// ====== Direcciones de fgamex86.dll (Ghidra - 0x10000000 = RVA) ======
static const DWORD RVA_TABLA_EXPORTS = 0x001B93F8;
static const DWORD RVA_G_ENTITIES = 0x001B9628;
static const DWORD CASILLA_CLIENTCONNECT = 0x14;
static const DWORD CASILLA_CLIENTBEGIN = 0x18;
static const DWORD RVA_CLIENTCONNECT = 0x00075310;
static const DWORD RVA_CLIENTBEGIN = 0x00073880;
static const DWORD RVA_RUNENTITY_PARCHE = 0x00078676;
static const DWORD RVA_RUNENTITY_RETORNO = 0x00078680;
static const DWORD RVA_ESTADO_PARCHE = 0x000B2E80;   // inicio de FUN_100b2e80
static const DWORD RVA_ESTADO_RETORNO = 0x000B2E86;
static const DWORD CASILLA_CLIENTDISC = 0x20;
static const DWORD CASILLA_CLIENTTHINK = 0x28;
static const DWORD RVA_CLIENTDISC = 0x00073980;
static const DWORD RVA_CLIENTTHINK = 0x00073800;
static const DWORD RVA_GUARDA_BEGIN = 0x003FA064;   // DAT_103fa064: ClientBegin hace "if (DAT_103fa064 == 0) FUN_100d2750();"
static const DWORD RVA_CINEMATICA = 0x001CD584;     // DAT_101cd584: se pone a 1 al empezar una cinematica (hipotesis)

// ---- Vuelca la ficha (gentity) de un jugador ----
static void DumpEdict(const char* tag, int idx) {
    BYTE* base = g_fgameBase;
    if (!base) return;
    __try {
        DWORD* t = (DWORD*)(base + RVA_TABLA_EXPORTS);
        BYTE* edicts = (BYTE*)t[0x54 / 4];
        DWORD size = t[0x58 / 4];
        if (!edicts || !size) { Log("[EDICT %d] %s: tabla sin entidades todavia", idx, tag); return; }
        BYTE* e = edicts + (size_t)idx * size;
        DWORD client = *(DWORD*)(e + 0x200);
        DWORD inuse = *(DWORD*)(e + 0x204);
        DWORD obj = *(DWORD*)(e + 0x280);
        DWORD next = *(DWORD*)(e + 0x2D4);
        Log("[EDICT %d] %s: gentity=%p client=%08X inuse=%u entity=%08X next=%08X",
            idx, tag, e, client, (unsigned)inuse, obj, next);
        if (obj)
            Log("[EDICT %d] %s:   Entity->edict(+0x10)=%08X (deberia ser %p)",
                idx, tag, *(DWORD*)(obj + 0x10), e);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[EDICT %d] %s: error al leer", idx, tag);
    }
}

// ---- Hook de ClientConnect ----
typedef const char* (__cdecl* PFN_ClientConnect)(int clientNum, int firstTime);
static PFN_ClientConnect g_origClientConnect = nullptr;
static char g_persNombre[120] = "";       // ultimo nombre de archivo pasado a ReadPersistant
static bool g_persTiene = false;
static bool g_cambioNivel0 = false;       // idem para el cliente 0 (para releer el inventario cuando juega A solo)
static bool g_hayCliente1 = false;        // true si el cliente 1 se conecto a este nivel
static bool g_cambioNivel = false;        // true tras un ClientConnect(firstTime=0) del cliente 1 (cambio de mapa)
typedef void(__cdecl* PFN_Persist)(DWORD arg);
static PFN_Persist g_origReadPers = nullptr, g_origWritePers = nullptr;
typedef void(__cdecl* PFN_ClientBegin)(DWORD ent);
typedef void(__cdecl* PFN_ClientThink)(DWORD ent, DWORD cmd);
typedef void(__cdecl* PFN_ClientDisc)(DWORD ent);
static PFN_ClientBegin g_origClientBegin = nullptr;
static PFN_ClientThink g_origClientThink = nullptr;
static PFN_ClientDisc  g_origClientDisc = nullptr;
static DWORD g_diagHasta = 0;      // GetTickCount() hasta el que se vuelca el diagnostico periodico
static DWORD g_diagProximo = 0;
static LONG  g_thinkCount[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static const char* __cdecl Detour_ClientConnect(int clientNum, int firstTime) {
    Log("[HOOK] ClientConnect(clientNum=%d, firstTime=%d)", clientNum, firstTime);
    if (clientNum >= 1) { g_cambioNivel = (firstTime == 0); g_hayCliente1 = true; }
    else { g_cambioNivel0 = (firstTime == 0); g_hayCliente1 = false; }
    DumpEdict("antes", clientNum);
    const char* r = g_origClientConnect ? g_origClientConnect(clientNum, firstTime) : "error del hook";
    Log("[HOOK] ClientConnect -> %s", r ? r : "NULL (aceptado)");
    DumpEdict("despues", clientNum);
    return r;
}

// ====== Vigilancia por hardware (puntos de interrupcion de datos) ======
static DWORD g_watchAddr[4] = { 0, 0, 0, 0 };
static const char* g_watchName[4] = { "", "", "", "" };
static int  g_watchCount = 0;
static LONG g_watchHits = 0;

static void SetWatch(const char* n0, DWORD a0, const char* n1, DWORD a1, const char* n2, DWORD a2) {
    g_watchCount = 0;
    if (a0) { g_watchAddr[g_watchCount] = a0; g_watchName[g_watchCount++] = n0; }
    if (a1) { g_watchAddr[g_watchCount] = a1; g_watchName[g_watchCount++] = n1; }
    if (a2) { g_watchAddr[g_watchCount] = a2; g_watchName[g_watchCount++] = n2; }
    g_watchHits = 0;

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    if (!GetThreadContext(th, &ctx)) { Log("[WATCH] GetThreadContext fallo (%lu)", GetLastError()); return; }

    DWORD* regs[4] = { &ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3 };
    DWORD dr7 = 0;
    for (int i = 0; i < g_watchCount; i++) {
        *regs[i] = g_watchAddr[i];
        dr7 |= (1u << (i * 2));        // activar
        dr7 |= (1u << (16 + i * 4));   // vigilar escrituras
        dr7 |= (3u << (18 + i * 4));   // 4 bytes
    }
    ctx.Dr7 = dr7;
    ctx.Dr6 = 0;
    if (SetThreadContext(th, &ctx)) {
        for (int i = 0; i < g_watchCount; i++)
            Log("[WATCH] vigilando %s en %08X", g_watchName[i], g_watchAddr[i]);
    }
    else {
        Log("[WATCH] SetThreadContext fallo (%lu)", GetLastError());
    }
}

// ====== Utilidades de entidades ======
// Devuelve el numero de entidad (0..1023) a partir de su direccion, o -1 si no es una entidad
static int EntIndex(DWORD ent) {
    if (!g_fgameBase) return -1;
    DWORD ge = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return -1;
    if (ent < ge) return -1;
    DWORD d = ent - ge;
    if ((d % 0x2DC) != 0) return -1;
    DWORD i = d / 0x2DC;
    return (i < 1024) ? (int)i : -1;
}

// Copia hasta n-1 caracteres imprimibles de una cadena del juego (sin fallar si la direccion no es valida)
static void SafeStr(DWORD addr, char* out, int n) {
    out[0] = 0;
    if (!addr) return;
    t_silencio++;
    __try {
        int i = 0;
        for (; i < n - 1; i++) {
            char c = *(const char*)(addr + i);
            if (c == 0) break;
            out[i] = (c >= 32 && c < 127) ? c : '?';
        }
        out[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '?'; out[1] = 0; }
    t_silencio--;
}

static char g_dumpPrev[2][200];

static void DumpOne(const char* tag, int n, DWORD ge) {
    DWORD e = ge + (DWORD)n * 0x2DC;
    DWORD client = 0, inuse = 0, obj = 0, vt = 0, A = 0, B = 0, S = 0;
    SafeRead((const DWORD*)(e + 0x200), &client);
    SafeRead((const DWORD*)(e + 0x204), &inuse);
    SafeRead((const DWORD*)(e + 0x280), &obj);
    if (obj) {
        SafeRead((const DWORD*)obj, &vt);
        SafeRead((const DWORD*)(obj + 0x340), &A);
        if (A) SafeRead((const DWORD*)(A + 0x1C), &B);
        if (B) SafeRead((const DWORD*)B, &S);
    }
    char nombre[40];
    SafeStr(S, nombre, (int)sizeof(nombre));
    DWORD vtRva = (g_fgameBase && vt >= (DWORD)g_fgameBase) ? vt - (DWORD)g_fgameBase : 0;
    DWORD f140 = 0, f17C = 0, f180 = 0, f34C = 0, f1C0 = 0, f1C4 = 0, ox = 0, oy = 0, oz = 0;
    if (obj) {   // campos que mira la busqueda de enemigos (FUN_100bd6e0 = FindClosestEnemyInRadius)
        SafeRead((const DWORD*)(obj + 0x140), &f140);
        SafeRead((const DWORD*)(obj + 0x17C), &f17C);
        SafeRead((const DWORD*)(obj + 0x180), &f180);
        SafeRead((const DWORD*)(obj + 0x34C), &f34C);
        SafeRead((const DWORD*)(obj + 0x1C0), &f1C0);
        SafeRead((const DWORD*)(obj + 0x1C4), &f1C4);
        SafeRead((const DWORD*)(obj + 0x6C), &ox);
        SafeRead((const DWORD*)(obj + 0x70), &oy);
        SafeRead((const DWORD*)(obj + 0x74), &oz);
    }
    // En el volcado periodico solo se registra si cambia algo relevante (estado, muerto, flags, equipo)
    char clave[200];
    sprintf_s(clave, "%08X %u %08X %s %08X %08X %08X", obj, (unsigned)inuse, A, nombre, f17C, f180, f34C);
    if (n >= 0 && n < 2) {
        bool periodico = strcmp(tag, "periodico") == 0;
        if (periodico && strcmp(g_dumpPrev[n], clave) == 0) return;
        strcpy_s(g_dumpPrev[n], clave);
    }
    Log("[JUG %d] %s: edict=%08X client=%08X inuse=%u obj=%08X vtable_rva=%08X estado=%08X nombre_estado=\"%s\"",
        n, tag, e, client, (unsigned)inuse, obj, vtRva, A, nombre);
    if (obj) {
        Log("[IA %d] %s: +140=%08X muerto(+17C)=%08X flags(+180)=%08X equipo(+34C)=%08X +1C0=%08X +1C4=%08X pos(+6C)=%08X %08X %08X",
            n, tag, f140, f17C, f180, f34C, f1C0, f1C4, ox, oy, oz);
    }
}

// Vuelca el estado de los jugadores 0 y 1 (entidades) y las banderas globales que nos interesan
static void DumpPlayers(const char* tag) {
    if (!g_fgameBase) return;
    DWORD ge = 0, numEd = 0, cin = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    SafeRead((const DWORD*)(g_fgameBase + RVA_TABLA_EXPORTS + 0x5C), &numEd);
    SafeRead((const DWORD*)(g_fgameBase + RVA_CINEMATICA), &cin);
    static DWORD pEd = 0, pCin = 0xFFFFFFFF, pGe = 0;
    bool periodico = strcmp(tag, "periodico") == 0;
    if (!periodico || ge != pGe || numEd != pEd || cin != pCin) {
        Log("[JUG] %s: g_entities=%08X num_edicts=%u flag_cinematica=%u", tag, ge, (unsigned)numEd, (unsigned)cin);
        pGe = ge; pEd = numEd; pCin = cin;
    }
    DumpOne(tag, 0, ge);
    DumpOne(tag, 1, ge);
}

// ---- Hook de ClientBegin (con diagnostico de las entidades 0 y 1) ----
static void __cdecl LogClientBegin(DWORD ent) {
    DWORD client = 0, inuse = 0, obj = 0;
    SafeRead((const DWORD*)(ent + 0x200), &client);
    SafeRead((const DWORD*)(ent + 0x204), &inuse);
    SafeRead((const DWORD*)(ent + 0x280), &obj);
    Log("[HOOK] ClientBegin(entidad=%08X) client=%08X inuse=%u entity=%08X", ent, client, (unsigned)inuse, obj);

    if (g_fgameBase) {
        DWORD ge = 0, obj0 = 0, A0 = 0;
        SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge);
        Log("[DIAG] g_entities=%08X", ge);
        for (int n = 0; n < 2; n++) {
            DWORD e = 0, A = 0, B = 0;
            if (ge && SafeRead((const DWORD*)(ge + n * 0x2DC + 0x280), &e) && e) {
                SafeRead((const DWORD*)(e + 0x340), &A);
                if (A) SafeRead((const DWORD*)(A + 0x1C), &B);
                Log("[DIAG]   entidad %d: objeto=%08X  [+0x340]=%08X  [[+0x340]+0x1C]=%08X", n, e, A, B);
                if (n == 0) { obj0 = e; A0 = A; }
            }
            else {
                Log("[DIAG]   entidad %d: sin objeto", n);
            }
        }
        // (opcional) vigilar los datos del estado del jugador 0 al entrar el cliente 1
        if (g_optWatchHW && ge && ent == ge + 0x2DC && obj0 && A0)
            SetWatch("A+0x1C", A0 + 0x1C, "obj0+0x340", obj0 + 0x340, "A+0x00", A0);
    }
}

// ---- Proteccion del cliente 1 (modo dios = bit 8 de obj+0x180, el mismo que usa el comando "god") y volcado de cambios ----
static DWORD g_protHasta = 0;      // GetTickCount() hasta el que el cliente 1 esta protegido (0 = no)
static DWORD g_protObj = 0;        // objeto Player protegido
static bool  g_protPuesta = false; // true si fuimos nosotros quienes pusimos el bit
static DWORD g_prev[2][0x100];
static bool  g_prevOk[2] = { false, false };
static DWORD g_diffProximo = 0;
static DWORD g_cambiosHasta = 0;

static void CopiarEquipo(DWORD ent) {
    if (!g_optEquipo || !g_fgameBase) return;
    DWORD ge = 0, obj0 = 0, obj1 = 0, t0 = 0, t1 = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    SafeRead((const DWORD*)(ge + 0x280), &obj0);
    SafeRead((const DWORD*)(ent + 0x280), &obj1);
    if (!obj0 || !obj1) return;
    if (!SafeRead((const DWORD*)(obj0 + 0x34C), &t0) || !SafeRead((const DWORD*)(obj1 + 0x34C), &t1)) return;
    if (t0 != t1) {
        *(DWORD*)(obj1 + 0x34C) = t0;
        Log("[EQUIPO] cliente 1: equipo %08X -> %08X (copiado del jugador 0)", t1, t0);
    }
    else {
        Log("[EQUIPO] cliente 1: el equipo ya coincide (%08X)", t0);
    }
}

static DWORD g_prot0Hasta = 0, g_prot0Obj = 0;
static bool  g_prot0Puesta = false;
// Modo dios del jugador 0 desde ANTES de que el cliente 1 aparezca (el spawn del cliente 1 lo mataba)
static void Proteccion0Antes() {
    if (g_optProt0Ms <= 0 || !g_fgameBase) return;
    DWORD ge = 0, obj0 = 0, fl = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    if (!SafeRead((const DWORD*)(ge + 0x280), &obj0) || !obj0) return;
    if (!SafeRead((const DWORD*)(obj0 + 0x180), &fl)) return;
    g_prot0Obj = obj0;
    g_prot0Puesta = (fl & 8) == 0;
    if (g_prot0Puesta) *(DWORD*)(obj0 + 0x180) = fl | 8;
    g_prot0Hasta = GetTickCount() + (DWORD)g_optProt0Ms;
    Log("[PROT0] jugador 0: modo dios activado ANTES del ClientBegin del cliente 1 (obj=%08X flags %08X -> %08X) durante %d ms", obj0, fl, fl | 8, g_optProt0Ms);
}
static void Proteccion0Revisar() {
    if (!g_prot0Hasta) return;
    if ((int)(g_prot0Hasta - GetTickCount()) > 0) return;
    DWORD fl = 0;
    if (g_prot0Obj && g_prot0Puesta && SafeRead((const DWORD*)(g_prot0Obj + 0x180), &fl)) {
        *(DWORD*)(g_prot0Obj + 0x180) = fl & ~8u;
        Log("[PROT0] jugador 0: modo dios desactivado (flags %08X -> %08X)", fl, fl & ~8u);
    }
    g_prot0Hasta = 0; g_prot0Obj = 0; g_prot0Puesta = false;
}

static void ProteccionAplicar(DWORD ent) {
    if (g_optProtegerMs <= 0) return;
    DWORD obj = 0, fl = 0;
    if (!SafeRead((const DWORD*)(ent + 0x280), &obj) || !obj) return;
    if (!SafeRead((const DWORD*)(obj + 0x180), &fl)) return;
    g_protObj = obj;
    g_protPuesta = (fl & 8) == 0;
    if (g_protPuesta) *(DWORD*)(obj + 0x180) = fl | 8;
    g_protHasta = GetTickCount() + (DWORD)g_optProtegerMs;
    Log("[PROT] cliente 1: modo dios activado en obj=%08X (flags %08X -> %08X) durante %d ms", obj, fl, fl | 8, g_optProtegerMs);
}
static void ProteccionRevisar() {
    if (!g_protHasta) return;
    if ((int)(g_protHasta - GetTickCount()) > 0) return;
    DWORD fl = 0;
    if (g_protObj && g_protPuesta && SafeRead((const DWORD*)(g_protObj + 0x180), &fl)) {
        *(DWORD*)(g_protObj + 0x180) = fl & ~8u;
        Log("[PROT] cliente 1: modo dios desactivado (flags %08X -> %08X)", fl, fl & ~8u);
    }
    g_protHasta = 0; g_protObj = 0; g_protPuesta = false;
}
// Compara los primeros 0x400 bytes de cada objeto Player con la lectura anterior y registra solo lo que cambia
static void DumpCambios() {
    if (!g_fgameBase) return;
    DWORD ge = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    for (int n = 0; n < 2; n++) {
        DWORD obj = 0;
        if (!SafeRead((const DWORD*)(ge + (DWORD)n * 0x2DC + 0x280), &obj) || !obj) { g_prevOk[n] = false; continue; }
        int cambios = 0;
        for (int i = 0; i < 0x100; i++) {
            DWORD v = 0;
            if (!SafeRead((const DWORD*)(obj + (DWORD)i * 4), &v)) continue;
            if (g_prevOk[n] && v != g_prev[n][i] && cambios < 24) {
                Log("[CAMBIO %d] +%03X: %08X -> %08X", n, i * 4, g_prev[n][i], v);
                cambios++;
            }
            g_prev[n][i] = v;
        }
        g_prevOk[n] = true;
    }
}


// ---- v0.19: escaner de entidades. Busca valores corruptos (NaN/infinito/enormes), saltos bruscos y entidades que nacen o mueren ----
static int  g_optEscaner = 1;       // EscanerEntidades: 1 = activo mientras dura CambiosDuracionMs
static bool g_escPrevInuse[1024];
static DWORD g_escPrevVt[1024];
static float g_escPrevPos[1024][3];
static bool g_escPrevOk = false;
static int  g_escLineas = 0;
static DWORD g_escUltimoInuse = 0xFFFFFFFF;
static bool EscMala(float f) { DWORD u = *(DWORD*)&f; if (((u >> 23) & 0xFF) == 0xFF) return true; return (f > 1.0e6f || f < -1.0e6f); }
static void EscanearEntidades() {
    if (!g_optEscaner || !g_fgameBase) return;
    DWORD ge = 0, numEd = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    SafeRead((const DWORD*)(g_fgameBase + RVA_TABLA_EXPORTS + 0x5C), &numEd);
    if (numEd > 1024) numEd = 1024;
    DWORD vivas = 0;
    for (DWORD i = 0; i < numEd; i++) {
        DWORD e = ge + i * 0x2DC, inuse = 0, obj = 0;
        SafeRead((const DWORD*)(e + 0x204), &inuse);
        bool uso = (inuse & 0xFF) != 0;
        if (uso) { SafeRead((const DWORD*)(e + 0x280), &obj); }
        float pos[3] = { 0, 0, 0 }; DWORD vt = 0;
        if (uso && obj) {
            SafeRead((const DWORD*)obj, &vt);
            for (int k = 0; k < 3; k++) { DWORD u = 0; if (SafeRead((const DWORD*)(obj + 0x90 + 4 * k), &u)) pos[k] = *(float*)&u; }
        }
        if (uso && obj) vivas++;
        if (g_escPrevOk && g_escLineas < 600) {
            if ((uso && obj) && !g_escPrevInuse[i]) {
                Log("[ESC] NUEVA ent %u obj=%08X vt_rva=%08X pos=%.1f %.1f %.1f", i, obj, vt ? vt - (DWORD)g_fgameBase : 0, pos[0], pos[1], pos[2]); g_escLineas++;
            }
            else if (!(uso && obj) && g_escPrevInuse[i]) {
                Log("[ESC] LIBERADA ent %u (vt_rva=%08X pos=%.1f %.1f %.1f)", i, g_escPrevVt[i] ? g_escPrevVt[i] - (DWORD)g_fgameBase : 0, g_escPrevPos[i][0], g_escPrevPos[i][1], g_escPrevPos[i][2]); g_escLineas++;
            }
            else if (uso && obj && g_escPrevInuse[i]) {
                bool mala = EscMala(pos[0]) || EscMala(pos[1]) || EscMala(pos[2]);
                bool malaAntes = EscMala(g_escPrevPos[i][0]) || EscMala(g_escPrevPos[i][1]) || EscMala(g_escPrevPos[i][2]);
                float dx = pos[0] - g_escPrevPos[i][0], dy = pos[1] - g_escPrevPos[i][1], dz = pos[2] - g_escPrevPos[i][2];
                if (mala && !malaAntes) { Log("[ESC] !!! VALOR CORRUPTO ent %u obj=%08X vt_rva=%08X pos=%g %g %g", i, obj, vt - (DWORD)g_fgameBase, pos[0], pos[1], pos[2]); g_escLineas++; }
                else if (!mala && !malaAntes && (dx * dx + dy * dy + dz * dz) > 500.0f * 500.0f) { Log("[ESC] salto brusco ent %u vt_rva=%08X (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)", i, vt - (DWORD)g_fgameBase, g_escPrevPos[i][0], g_escPrevPos[i][1], g_escPrevPos[i][2], pos[0], pos[1], pos[2]); g_escLineas++; }
            }
        }
        g_escPrevInuse[i] = uso && obj; g_escPrevVt[i] = vt; memcpy(g_escPrevPos[i], pos, sizeof(pos));
    }
    if (vivas != g_escUltimoInuse) { Log("[ESC] entidades vivas = %u (num_edicts=%u)", vivas, numEd); g_escUltimoInuse = vivas; }
    g_escPrevOk = true;
}


// ---- v0.21: busca NaN/infinito en TODOS los campos de cada entidad (edict + objeto) y describe las entidades clave ----
static unsigned char g_escMalo[1024][183 + 0x200];
static int g_escLineas2 = 0;
static DWORD g_clasifProximo = 0;
static bool EsNanInf(DWORD u) { if (u == 0xFFFFFFFFu || u == 0xFFFFFFFEu) return false; return ((u >> 23) & 0xFF) == 0xFF; }
static void EscanearCampos() {
    if (!g_optEscaner || !g_fgameBase) return;
    DWORD ge = 0, numEd = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    SafeRead((const DWORD*)(g_fgameBase + RVA_TABLA_EXPORTS + 0x5C), &numEd);
    if (numEd > 1024) numEd = 1024;
    for (DWORD i = 0; i < numEd; i++) {
        DWORD e = ge + i * 0x2DC, inuse = 0, obj = 0;
        SafeRead((const DWORD*)(e + 0x204), &inuse);
        if (!(inuse & 0xFF)) { memset(g_escMalo[i], 0, sizeof(g_escMalo[i])); continue; }
        SafeRead((const DWORD*)(e + 0x280), &obj);
        DWORD vt = 0; if (obj) SafeRead((const DWORD*)obj, &vt);
        for (int k = 0; k < 183 + 0xC0; k++) {
            DWORD u = 0; bool ok;
            if (k < 183) ok = SafeRead((const DWORD*)(e + (DWORD)k * 4), &u);
            else { if (!obj) break; ok = SafeRead((const DWORD*)(obj + (DWORD)(k - 183) * 4), &u); }
            if (!ok) continue;
            bool mal = EsNanInf(u);
            if (mal && !g_escMalo[i][k] && g_escLineas2 < 300) {
                if (k < 183) Log("[ESC2] !!! NaN/Inf ent %u en el edict +%03X = %08X (vt_rva=%08X)", i, k * 4, u, vt ? vt - (DWORD)g_fgameBase : 0);
                else Log("[ESC2] !!! NaN/Inf ent %u en el objeto +%03X = %08X (vt_rva=%08X)", i, (k - 183) * 4, u, vt ? vt - (DWORD)g_fgameBase : 0);
                g_escLineas2++;
            }
            g_escMalo[i][k] = mal;
        }
    }
}
// Lee una cadena ASCII corta de una direccion si parece texto
static bool LeerTexto(DWORD addr, char* out, int max) {
    if (addr < 0x10000 || addr > 0x7FFE0000) return false;
    int n = 0;
    for (; n < max - 1; n++) {
        if (!SafeReadByte(addr + n)) return false;
        char c = *(char*)(addr + n);
        if (c == 0) break;
        if (c < 32 || c > 126) return false;
        out[n] = c;
    }
    out[n] = 0;
    return n >= 4;
}
static void DescribirEntidad(DWORD i) {
    DWORD ge = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    DWORD e = ge + i * 0x2DC, inuse = 0, obj = 0;
    SafeRead((const DWORD*)(e + 0x204), &inuse);
    SafeRead((const DWORD*)(e + 0x280), &obj);
    DWORD vt = 0; if (obj) SafeRead((const DWORD*)obj, &vt);
    float p[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) { DWORD u = 0; if (obj && SafeRead((const DWORD*)(obj + 0x90 + 4 * k), &u)) p[k] = *(float*)&u; }
    Log("[ENT %u] inuse=%u obj=%08X vt_rva=%08X pos=%.1f %.1f %.1f", i, inuse & 0xFF, obj, vt ? vt - (DWORD)g_fgameBase : 0, p[0], p[1], p[2]);
    if (!obj || !(inuse & 0xFF)) return;
    char t[64];
    for (int k = 0; k < 0x200; k++) {
        DWORD w = 0;
        if (!SafeRead((const DWORD*)(obj + (DWORD)k * 4), &w)) continue;
        if (LeerTexto(w, t, sizeof(t))) Log("[ENT %u]   obj+%03X -> \"%s\"", i, k * 4, t);
        // tambien dos niveles: puntero a un objeto que a su vez apunta a texto en sus primeros campos
    }
    for (int k = 0; k < 183; k++) {
        DWORD w = 0;
        if (!SafeRead((const DWORD*)(e + (DWORD)k * 4), &w)) continue;
        if (LeerTexto(w, t, sizeof(t))) Log("[ENT %u]   edict+%03X -> \"%s\"", i, k * 4, t);
    }
}
static void DescribirClave() {
    static const DWORD lista[] = { 0, 1, 65, 110, 170, 277, 278, 279, 280, 281 };
    Log("[ENT] ---- descripcion de entidades clave ----");
    for (int n = 0; n < (int)(sizeof(lista) / sizeof(lista[0])); n++) DescribirEntidad(lista[n]);
}


// ---- v0.22: EXPERIMENTO. Elimina una entidad concreta (con su destructor virtual, igual que hace ClientDisconnect con el Player)
// ExperimentoEntidad=N (0 = desactivado), ExperimentoMs = milisegundos tras el ClientBegin del cliente 1 ----
static int   g_optExpEnt = 0;
static int   g_optExpMs = 15000;
static DWORD g_expAt = 0;
typedef void(__thiscall* PFN_Dtor)(void* self, int flags);
static void ExperimentoRevisar() {
    if (!g_expAt || !g_optExpEnt || !g_fgameBase) return;
    if ((int)(GetTickCount() - g_expAt) < 0) return;
    g_expAt = 0;
    DWORD ge = 0, obj = 0, inuse = 0, vt = 0, fn = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    DWORD e = ge + (DWORD)g_optExpEnt * 0x2DC;
    SafeRead((const DWORD*)(e + 0x204), &inuse);
    SafeRead((const DWORD*)(e + 0x280), &obj);
    if (!(inuse & 0xFF) || !obj) { Log("[EXPERIMENTO] ent %d no esta en uso, no se hace nada", g_optExpEnt); return; }
    if (!SafeRead((const DWORD*)obj, &vt) || vt < (DWORD)g_fgameBase || vt >(DWORD)g_fgameBase + 0x420000) { Log("[EXPERIMENTO] ent %d: vtable fuera de fgame", g_optExpEnt); return; }
    if (!SafeRead((const DWORD*)(vt + 4), &fn) || fn < (DWORD)g_fgameBase || fn >(DWORD)g_fgameBase + 0x420000) { Log("[EXPERIMENTO] ent %d: destructor invalido", g_optExpEnt); return; }
    Log("[EXPERIMENTO] eliminando ent %d obj=%08X vt_rva=%08X destructor=%08X", g_optExpEnt, obj, vt - (DWORD)g_fgameBase, fn);
    ((PFN_Dtor)fn)((void*)obj, 1);
    DWORD o2 = 0;
    if (SafeRead((const DWORD*)(e + 0x280), &o2) && o2 == obj) *(DWORD*)(e + 0x280) = 0;
    Log("[EXPERIMENTO] ent %d eliminada", g_optExpEnt);
}


// ---- v0.23: CENTINELA DE CINEMATICAS. Para los mapas de la lista envia "skipcinematic" (lo mismo que hace la tecla Esc)
// SaltarCinematicaMapas = lista separada por comas con el nombre del mapa tal como sale en [NIVEL] (vacio = desactivado)
// SaltarCinematicaMs    = milisegundos tras el ClientBegin del cliente 1 en que se envia el comando ----
static char  g_optSkipMapas[256] = "fortress1$fortress1_start1";
static int   g_optSkipMs = 35000;
static DWORD g_skipAt = 0;
static bool MapaEnLista() {
    BYTE* exe = (BYTE*)GetModuleHandleA(NULL);
    DWORD cv = 0, str = 0;
    if (!exe || !SafeRead((const DWORD*)(exe + (0x01275E90 - 0x00400000)), &cv) || !cv) return false;
    if (!SafeRead((const DWORD*)(cv + 4), &str) || !str) return false;
    char m[96] = { 0 };
    SafeStr(str, m, (int)sizeof(m));
    if (!m[0] || !g_optSkipMapas[0]) return false;
    char lista[256]; strcpy_s(lista, g_optSkipMapas);
    char* ctx = nullptr;
    for (char* t = strtok_s(lista, ",", &ctx); t; t = strtok_s(nullptr, ",", &ctx)) {
        while (*t == ' ') t++;
        if (strcmp(t, m) == 0) { Log("[CENTINELA] el mapa \"%s\" esta en la lista: se saltara la cinematica a los %d ms", m, g_optSkipMs); return true; }
    }
    return false;
}
typedef void(__cdecl* PFN_CbufAddText)(const char* texto);   // FUN_004158f0 de alice.exe
static DWORD g_tpAt = 0;   // v0.24: tras saltar la cinematica se lleva al cliente 1 junto al jugador 0
static void SepararSpawn(DWORD ent);
static void CentinelaRevisar() {
    if (!g_skipAt) return;
    if ((int)(GetTickCount() - g_skipAt) < 0) return;
    g_skipAt = 0;
    BYTE* exe = (BYTE*)GetModuleHandleA(NULL);
    if (!exe) return;
    Log("[CENTINELA] enviando skipcinematic");
    ((PFN_CbufAddText)(exe + (0x004158F0 - 0x00400000)))("skipcinematic\n");
    g_tpAt = GetTickCount() + 2500;
}

static void SepararSpawn(DWORD ent);   // definida mas abajo

static void __cdecl Detour_ClientBegin(DWORD ent) {
    LogClientBegin(ent);
    int idx = EntIndex(ent);

    // Opcion SaltarResetClientes: ClientBegin ejecuta "if (DAT_103fa064 == 0) FUN_100d2750();".
    // Para los clientes 1 en adelante ponemos esa variable a 1 durante la llamada, asi se salta esa funcion.
    DWORD* guarda = g_fgameBase ? (DWORD*)(g_fgameBase + RVA_GUARDA_BEGIN) : nullptr;
    DWORD guardaOrig = 0;
    bool tocada = false;
    if (g_optSaltarReset && idx >= 1 && guarda && SafeRead(guarda, &guardaOrig)) {
        *guarda = 1;
        tocada = true;
        Log("[HOOK] ClientBegin: SaltarResetClientes activo (DAT_103fa064 %u -> 1 durante la llamada)", (unsigned)guardaOrig);
    }

    if (idx >= 1) Proteccion0Antes();
    if (g_origClientBegin) g_origClientBegin(ent);
    if (tocada) *guarda = guardaOrig;

    Log("[HOOK] ClientBegin(cliente %d) terminado", idx);
    DumpPlayers("despues de ClientBegin");
    // v0.20: si juega A solo (sin cliente 1) el inventario tambien se pierde en el cambio de mapa; se relee aqui
    if (idx == 0 && g_optRelectura && g_cambioNivel0 && !g_hayCliente1 && g_persTiene && g_origReadPers) {
        Log("[PERSIST] relectura tras el ClientBegin del cliente 0 (sin cliente 1): ReadPersistant(\"%s\")", g_persNombre);
        g_origReadPers((DWORD)g_persNombre);
        DumpPlayers("relectura cliente 0 despues");
    }
    if (idx == 0) g_cambioNivel0 = false;
    if (idx >= 1) {
        CopiarEquipo(ent);
        ProteccionAplicar(ent);
        // En el cambio de mapa el motor llama a ReadPersistant ANTES de que existan los jugadores, asi que no se aplica a nadie.
        // Ahora que los dos existen, se vuelve a leer: el archivo trae un registro por jugador, en el orden 0, 1.
        if (g_optRelectura && g_cambioNivel && g_persTiene && g_origReadPers) {
            Log("[PERSIST] relectura tras el ClientBegin del cliente 1: ReadPersistant(\"%s\")", g_persNombre);
            g_origReadPers((DWORD)g_persNombre);
            DumpPlayers("relectura despues");
        }
        g_cambioNivel = false;
        SepararSpawn(ent);
        g_prevOk[0] = g_prevOk[1] = false;
        g_escPrevOk = false; g_escLineas = 0; g_escUltimoInuse = 0xFFFFFFFF; g_escLineas2 = 0; memset(g_escMalo, 0, sizeof(g_escMalo)); g_clasifProximo = GetTickCount() + 4000; if (g_optExpEnt) g_expAt = GetTickCount() + (DWORD)g_optExpMs; g_skipAt = 0; if (MapaEnLista()) g_skipAt = GetTickCount() + (DWORD)g_optSkipMs;
        g_cambiosHasta = GetTickCount() + (DWORD)g_optCambiosMs;
        DWORD now = GetTickCount();
        g_diagHasta = now + (DWORD)g_optDiagDurMs;
        g_diagProximo = now + (DWORD)g_optDiagMs;
    }
}

// ---- Hook de ClientThink: cuenta las llamadas de cada cliente (llega su entrada al servidor?) ----
// ---- Seguir al jugador 0: si el cliente 1 se aleja mucho (p. ej. A se va en una cinematica) se le teletransporta ----
// Usa el metodo virtual +0x28 del objeto (el mismo que usa la rutina de teletransporte del juego: setOrigin(x, y, z))
typedef void(__thiscall* PFN_SetOrigin)(void* self, float x, float y, float z);
static DWORD g_seguirProximo = 0;
static void Seguir(DWORD ent) {
    if (!g_optSeguir || !g_fgameBase) return;
    DWORD now = GetTickCount();
    if ((int)(now - g_seguirProximo) < 0) return;
    DWORD ge = 0, obj0 = 0, obj1 = 0, m0 = 0, m1 = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    SafeRead((const DWORD*)(ge + 0x280), &obj0);
    SafeRead((const DWORD*)(ent + 0x280), &obj1);
    if (!obj0 || !obj1 || obj0 == obj1) return;
    if (!SafeRead((const DWORD*)(obj0 + 0x17C), &m0) || !SafeRead((const DWORD*)(obj1 + 0x17C), &m1)) return;
    if (m0 != 0 || m1 != 0) return;                         // solo si los dos estan vivos
    DWORD a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        if (!SafeRead((const DWORD*)(obj0 + 0x90 + 4 * i), &a[i]) || !SafeRead((const DWORD*)(obj1 + 0x90 + 4 * i), &b[i])) return;
    }
    float fa[3], fb[3];
    memcpy(fa, a, 12); memcpy(fb, b, 12);
    float dx = fa[0] - fb[0], dy = fa[1] - fb[1], dz = fa[2] - fb[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    float lim = (float)g_optSeguirDist;
    if (!(d2 > lim * lim)) return;
    DWORD vt = 0, fn = 0;
    if (!SafeRead((const DWORD*)obj1, &vt) || !vt || !SafeRead((const DWORD*)(vt + 0x28), &fn)) return;
    if (fn < (DWORD)g_fgameBase || fn >(DWORD)g_fgameBase + 0x420000) return;   // debe ser codigo de fgamex86.dll
    g_seguirProximo = now + (DWORD)g_optSeguirMs;
    Log("[SEGUIR] cliente 1 lejos del jugador 0 (distancia^2=%.0f): setOrigin(%.1f, %.1f, %.1f) fn=%08X", d2, fa[0] + 40.0f, fa[1], fa[2] + 8.0f, fn);
    ((PFN_SetOrigin)fn)((void*)obj1, fa[0] + 40.0f, fa[1], fa[2] + 8.0f);
}

// ---- SepararSpawn: tras el ClientBegin del cliente 1 se coloca junto al jugador 0 (no encima) con el mismo setOrigin ----
static void SepararSpawn(DWORD ent) {
    if (!g_optSepararSpawn || !g_fgameBase) return;
    DWORD ge = 0, obj0 = 0, obj1 = 0;
    if (!SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge) || !ge) return;
    SafeRead((const DWORD*)(ge + 0x280), &obj0);
    SafeRead((const DWORD*)(ent + 0x280), &obj1);
    if (!obj0 || !obj1 || obj0 == obj1) return;
    DWORD a[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) if (!SafeRead((const DWORD*)(obj0 + 0x90 + 4 * i), &a[i])) return;
    float fa[3]; memcpy(fa, a, 12);
    DWORD vt = 0, fn = 0;
    if (!SafeRead((const DWORD*)obj1, &vt) || !vt || !SafeRead((const DWORD*)(vt + 0x28), &fn)) return;
    if (fn < (DWORD)g_fgameBase || fn >(DWORD)g_fgameBase + 0x420000) return;
    float d = (float)g_optSepararDist;
    Log("[SEPARAR] cliente 1: setOrigin(%.1f, %.1f, %.1f) fn=%08X (jugador 0 en %.1f, %.1f, %.1f)", fa[0] + d, fa[1], fa[2] + 8.0f, fn, fa[0], fa[1], fa[2]);
    ((PFN_SetOrigin)fn)((void*)obj1, fa[0] + d, fa[1], fa[2] + 8.0f);
    DumpPlayers("despues de separar");
}

// ---- Persistencia del inventario entre mapas (ReadPersistant / WritePersistant) ----
static void LogPersist(const char* que, DWORD arg) {
    char nom[80];
    SafeStr(arg, nom, (int)sizeof(nom));
    Log("[PERSIST] %s(\"%s\")", que, nom);
    DumpPlayers(que);
}
static void __cdecl Detour_ReadPers(DWORD arg) {
    SafeStr(arg, g_persNombre, (int)sizeof(g_persNombre));
    g_persTiene = g_persNombre[0] != 0;
    LogPersist("ReadPersistant antes", arg);
    if (g_origReadPers) g_origReadPers(arg);
    DumpPlayers("ReadPersistant despues");
}
static void __cdecl Detour_WritePers(DWORD arg) {
    LogPersist("WritePersistant antes", arg);
    if (g_origWritePers) g_origWritePers(arg);
    Log("[PERSIST] WritePersistant terminado");
}

static void __cdecl Detour_ClientThink(DWORD ent, DWORD cmd) {
    int idx = EntIndex(ent);
    if (idx >= 0 && idx < 8) {
        LONG n = InterlockedIncrement(&g_thinkCount[idx]);
        if (idx >= 1 && (n <= 3 || (n % 250) == 0))
            Log("[THINK] cliente %d: llamada #%ld", idx, n);
    }
    if (g_origClientThink) g_origClientThink(ent, cmd);
    if (idx >= 1) { Seguir(ent); ExperimentoRevisar(); }
    CentinelaRevisar();
    if (idx >= 1 && g_tpAt && (int)(GetTickCount() - g_tpAt) >= 0) { g_tpAt = 0; Log("[CENTINELA] llevando al cliente 1 junto al jugador 0 tras saltar la cinematica"); SepararSpawn(ent); }
}

// ---- Hook de ClientDisconnect ----
static void __cdecl Detour_ClientDisc(DWORD ent) {
    int idx = EntIndex(ent);
    Log("[HOOK] ClientDisconnect(entidad=%08X, cliente %d)", ent, idx);
    DumpPlayers("antes de ClientDisconnect");
    if (g_origClientDisc) g_origClientDisc(ent);
    Log("[HOOK] ClientDisconnect terminado");
}

// ====== PARCHE 1: G_RunEntity (objetos sin edict) ======
static DWORD g_retRunEntity = 0;
static LONG  g_skipCount = 0;

static void __cdecl LogSkipRunEntity(DWORD obj) {
    LONG n = InterlockedIncrement(&g_skipCount);
    if (n <= 10 || (n % 500) == 0) {
        DWORD vt = 0;
        SafeRead((const DWORD*)obj, &vt);
        DWORD rva = (g_fgameBase && vt >= (DWORD)g_fgameBase) ? vt - (DWORD)g_fgameBase : 0;
        Log("[PARCHE] G_RunEntity: objeto %08X sin edict, se omite (n=%ld) vtable=%08X (RVA %08X)",
            obj, n, vt, rva);
    }
}

static __declspec(naked) void Patch_RunEntity() {
    __asm {
        mov  edi, dword ptr[esi + 0x10]
        test edi, edi
        jz   omitir
        cmp  dword ptr[edi + 0x204], 0
        jmp  dword ptr[g_retRunEntity]
        omitir:
        pushad
            push esi
            call LogSkipRunEntity
            add  esp, 4
            popad
            pop  edi
            pop  esi
            ret
    }
}

static void ApplyCodePatch(BYTE* base) {
    if (!PARCHE_RUNENTITY) return;
    BYTE* target = base + RVA_RUNENTITY_PARCHE;
    static const BYTE original[10] = { 0x8B, 0x7E, 0x10, 0x83, 0xBF, 0x04, 0x02, 0x00, 0x00, 0x00 };
    BYTE actual[10];
    __try { memcpy(actual, target, 10); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (actual[0] == 0xE9) return;
    if (memcmp(actual, original, 10) != 0) {
        static DWORD avisado = 0;
        if (avisado != (DWORD)base) {
            Log("Parche RunEntity: bytes inesperados en %p; no se aplica", target);
            avisado = (DWORD)base;
        }
        return;
    }
    g_retRunEntity = (DWORD)(base + RVA_RUNENTITY_RETORNO);
    BYTE parche[10] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(DWORD*)(parche + 1) = (DWORD)&Patch_RunEntity - (DWORD)(target + 5);
    DWORD oldProt = 0;
    if (VirtualProtect(target, 10, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(target, parche, 10);
        VirtualProtect(target, 10, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, 10);
        Log("Parche RunEntity aplicado en %p (salto a %p)", target, (void*)&Patch_RunEntity);
    }
}

// ====== PARCHE 2: FUN_100b2e80 (poner el estado "STAND" del jugador) ======
static DWORD g_retEstado = 0;
static LONG  g_skipEstado = 0;

// Devuelve 1 si la cadena de punteros del estado actual se puede leer hasta el primer caracter del nombre
static int __cdecl EstadoValido(DWORD self) {
    DWORD A = 0, B = 0, S = 0;
    if (!SafeRead((const DWORD*)(self + 0x340), &A) || !A) return 0;
    if (!SafeRead((const DWORD*)(A + 0x1C), &B) || !B) return 0;
    if (!SafeRead((const DWORD*)B, &S) || !S) return 0;
    if (!SafeReadByte(S)) return 0;
    return 1;
}

static void __cdecl LogSkipEstado(DWORD self) {
    LONG n = InterlockedIncrement(&g_skipEstado);
    if (n <= 10 || (n % 100) == 0) {
        DWORD A = 0, B = 0, S = 0;
        SafeRead((const DWORD*)(self + 0x340), &A);
        if (A) SafeRead((const DWORD*)(A + 0x1C), &B);
        if (B) SafeRead((const DWORD*)B, &S);
        Log("[PARCHE] estado STAND: objeto %08X con estado invalido (A=%08X B=%08X [B]=%08X), se omite (n=%ld)",
            self, A, B, S, n);
    }
}

static __declspec(naked) void Patch_Estado() {
    __asm {
        pushad
        push ecx
        call EstadoValido
        add  esp, 4
        test eax, eax
        popad
        jz   omitir2
        mov  eax, dword ptr fs : [0]
        jmp  dword ptr[g_retEstado]
        omitir2 :
        pushad
        push ecx
        call LogSkipEstado
        add  esp, 4
        popad
        ret  8
    }
}

static void ApplyStatePatch(BYTE* base) {
    if (!PARCHE_ESTADO) return;
    BYTE* target = base + RVA_ESTADO_PARCHE;
    static const BYTE original[8] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF };
    BYTE actual[8];
    __try { memcpy(actual, target, 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (actual[0] == 0xE9) return;
    if (memcmp(actual, original, 8) != 0) {
        static DWORD avisado = 0;
        if (avisado != (DWORD)base) {
            Log("Parche estado: bytes inesperados en %p; no se aplica", target);
            avisado = (DWORD)base;
        }
        return;
    }
    g_retEstado = (DWORD)(base + RVA_ESTADO_RETORNO);
    BYTE parche[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(DWORD*)(parche + 1) = (DWORD)&Patch_Estado - (DWORD)(target + 5);
    DWORD oldProt = 0;
    if (VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(target, parche, 6);
        VirtualProtect(target, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, 6);
        Log("Parche estado aplicado en %p (salto a %p)", target, (void*)&Patch_Estado);
    }
}

// ====== Parche de la tabla de funciones ======
// ====== PARCHE 3: no destruir el mapa de estados anterior (FUN_10059c60) ======
static const bool PARCHE_STATEMAP = true;
static const DWORD RVA_STATEMAP_JZ = 0x00059D21;

static void ApplyStatemapPatch(BYTE* base) {
    if (!PARCHE_STATEMAP) return;
    BYTE* target = base + RVA_STATEMAP_JZ;
    // 74 09 | 8B 11 | 8B 42 04 | 6A 01 | FF D0  (JZ que salta la llamada virtual de borrado)
    static const BYTE original[11] = { 0x74, 0x09, 0x8B, 0x11, 0x8B, 0x42, 0x04, 0x6A, 0x01, 0xFF, 0xD0 };
    BYTE actual[11];
    __try { memcpy(actual, target, 11); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (actual[0] == 0xEB && memcmp(actual + 1, original + 1, 10) == 0) return;   // ya parcheado
    if (memcmp(actual, original, 11) != 0) {
        static DWORD avisado = 0;
        if (avisado != (DWORD)base) {
            Log("Parche StateMap: bytes inesperados en %p; no se aplica", target);
            avisado = (DWORD)base;
        }
        return;
    }
    DWORD oldProt = 0;
    if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
        *target = 0xEB;
        VirtualProtect(target, 1, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), target, 1);
        Log("Parche StateMap aplicado en %p (el mapa de estados anterior ya no se destruye)", target);
    }
}
struct SlotPatch {
    const char* nombre;
    DWORD casilla;
    DWORD rvaEsperada;
    DWORD desvio;
    DWORD* original;
    DWORD ultimoMalo;
    const int* activo;   // si no es nulo y vale 0, la casilla no se parchea
};

static SlotPatch g_patches[6] = {
    { "ClientConnect",    CASILLA_CLIENTCONNECT, RVA_CLIENTCONNECT, 0, nullptr, 0, nullptr },
    { "ClientBegin",      CASILLA_CLIENTBEGIN,   RVA_CLIENTBEGIN,   0, nullptr, 0, nullptr },
    { "ClientThink",      CASILLA_CLIENTTHINK,   RVA_CLIENTTHINK,   0, nullptr, 0, &g_optHookThink },
    { "ClientDisconnect", CASILLA_CLIENTDISC,    RVA_CLIENTDISC,    0, nullptr, 0, &g_optHookDisc },
    { "ReadPersistant",   0x44,                  0x000734C0,        0, nullptr, 0, &g_optPersist },
    { "WritePersistant",  0x40,                  0x00073530,        0, nullptr, 0, &g_optPersist },
};

static void ApplyPatches(BYTE* base) {
    for (int i = 0; i < 6; i++) {
        SlotPatch& p = g_patches[i];
        if (p.activo && !*p.activo) continue;
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

static LONG g_crashLogs = 0;

static LONG CALLBACK CrashLogger(PEXCEPTION_POINTERS ep) {
    if (t_silencio > 0) return EXCEPTION_CONTINUE_SEARCH;
    // ---- Aviso de la vigilancia por hardware ----
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && g_watchCount > 0) {
        CONTEXT* wc = ep->ContextRecord;
        DWORD dr6 = wc->Dr6;
        if (dr6 & 0xF) {
            LONG n = InterlockedIncrement(&g_watchHits);
            if (n <= 15) {
                char d[160];
                DescribeAddress(wc->Eip, d, sizeof(d));
                for (int i = 0; i < g_watchCount; i++) {
                    if (!(dr6 & (1u << i))) continue;
                    DWORD now = 0;
                    SafeRead((const DWORD*)g_watchAddr[i], &now);
                    Log("[WATCH] escritura en %s (%08X): valor ahora %08X; EIP justo despues = %s",
                        g_watchName[i], g_watchAddr[i], now, d);
                }
                DWORD* wsp = (DWORD*)wc->Esp;
                int whits = 0;
                for (int i = 0; i < 1024 && whits < 30; i++) {
                    DWORD v = 0;
                    if (!SafeRead(wsp + i, &v)) break;
                    if (!IsCodeAddress(v)) continue;
                    char dd[160];
                    DescribeAddress(v, dd, sizeof(dd));
                    if (strstr(dd, "alice.exe") || strstr(dd, "fgamex86")) {
                        Log("[WATCH]   pila[+%03X] = %s", i * 4, dd);
                        whits++;
                    }
                }
            }
            if (n >= 15) wc->Dr7 = 0;     // desactivar tras 15 avisos
            wc->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW && g_log) {   // sin apenas pila: solo un aviso corto
        fputs("[EXCEPCION] DESBORDE DE PILA (stack overflow)\n", g_log);
        fflush(g_log);
    }
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

    DWORD vt = 0;
    if (SafeRead((const DWORD*)c->Esi, &vt))
        Log("[EXCEPCION] [ESI] = %08X (si ESI es un objeto C++, es su vtable)", vt);

    DWORD* sp = (DWORD*)c->Esp;

    if (g_fgameBase) {
        DWORD* t = (DWORD*)(g_fgameBase + RVA_TABLA_EXPORTS);
        DWORD edicts = 0, size = 0;
        if (SafeRead(t + 0x54 / 4, &edicts) && SafeRead(t + 0x58 / 4, &size) && edicts && size) {
            for (int i = 0; i < 16; i++) {
                DWORD v = 0;
                if (!SafeRead(sp + i, &v)) break;
                if (v >= edicts && v < edicts + 1024 * size)
                    Log("[EXCEPCION]   pila[+%02X] = %08X -> entidad #%u", i * 4, v, (v - edicts) / size);
            }
        }

        // ---- Diagnostico: que es ESI y a donde llevan los punteros del estado ----
        DWORD ge = 0;
        SafeRead((const DWORD*)(g_fgameBase + RVA_G_ENTITIES), &ge);
        Log("[DIAG] g_entities=%08X", ge);
        for (int n = 0; n < 2; n++) {
            DWORD e = 0;
            if (ge && SafeRead((const DWORD*)(ge + n * 0x2DC + 0x280), &e))
                Log("[DIAG] g_entities[%d].entity = %08X %s", n, e, e == c->Esi ? "<-- es ESI" : "");
        }
        DWORD A = 0, B = 0, S = 0;
        if (SafeRead((const DWORD*)(c->Esi + 0x340), &A)) {
            Log("[DIAG] ESI+0x340 (A) = %08X", A);
            if (SafeRead((const DWORD*)(A + 0x1C), &B)) {
                char d[160];
                DescribeAddress(B, d, sizeof(d));
                Log("[DIAG] [A+0x1C] (B) = %08X  (en %s)", B, d);
                if (SafeRead((const DWORD*)B, &S)) Log("[DIAG] [B] = %08X", S);
            }
            for (int i = 0; i < 12; i++) {
                DWORD v = 0;
                if (SafeRead((const DWORD*)(A + i * 4), &v)) Log("[DIAG] A[+%02X] = %08X", i * 4, v);
            }
        }
    }

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

// ---- Arreglo del cliente: el arbol de sectores (SV_AreaEntities) solo lo construye el servidor ----
// En un cliente puro (sin fgamex86.dll) la raiz DAT_011d4b00 esta a cero: el motor la trata como un nodo
// interno con hijos NULL y falla en alice.exe+0x30C00 (lectura de 0x10). Marcarla como hoja (-1) = "sin entidades".
static const DWORD RVA_ARBOL_RAIZ = 0x011D4B00 - 0x00400000;
static bool g_arbolLogueado = false;

// ---- v0.25: PERMITIR CARGAR PARTIDA CON sv_maxclients 2 ----
// El comando "loadgame" del motor (alice.exe 0x0042A2D0) hace: if (sv_maxclients > 1) return;  (en silencio)
// Es el salto "jg" de 6 bytes en 0x0042A2F9 (0F 8F 70 01 00 00). Se reemplaza por 6 NOP.
static int g_optPermitirCargar = 1;
static void ParchearLoadgame(BYTE* exeBase) {
    if (!g_optPermitirCargar) return;
    BYTE* p = exeBase + (0x0042A2F9 - 0x00400000);
    static const BYTE esperado[6] = { 0x0F, 0x8F, 0x70, 0x01, 0x00, 0x00 };
    if (memcmp(p, esperado, 6) != 0) {
        bool yaParcheado = true;
        for (int i = 0; i < 6; i++) if (p[i] != 0x90) yaParcheado = false;
        Log("[CARGAR] loadgame: %s", yaParcheado ? "ya estaba parcheado" : "los bytes no coinciden, no se toca (otra version de alice.exe?)");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) { Log("[CARGAR] VirtualProtect fallo (%lu)", GetLastError()); return; }
    memset(p, 0x90, 6);
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    Log("[CARGAR] loadgame parcheado en %p: ahora se puede cargar partida con sv_maxclients > 1", p);
}

static void ArreglarArbolCliente(BYTE* exeBase) {
    DWORD* raiz = (DWORD*)(exeBase + RVA_ARBOL_RAIZ);
    DWORD v = 0;
    if (!SafeRead(raiz, &v)) return;
    if (v == 0) {
        *raiz = (DWORD)-1;
        if (!g_arbolLogueado) { Log("Cliente sin servidor local: arbol de sectores marcado como vacio en %p", (void*)raiz); g_arbolLogueado = true; }
    }
}

// ---- Nombre del mapa actual (cvar "mapname" del motor: puntero en alice.exe+0xE75E90, cadena en +4) ----
static char g_mapaPrev[64] = "";
// v0.24: nombres que usa el motor para guardar/cargar partida (DAT_01275d88 = nombre del guardado, DAT_01275dc8 = nivel del guardado, DAT_01275f00 = ultimo mapa)
static char g_guardPrev[200] = "";
static void LogGuardado(BYTE* exeBase) {
    char a[65] = { 0 }, b[65] = { 0 }, c[65] = { 0 };
    SafeStr((DWORD)(exeBase + (0x01275D88 - 0x00400000)), a, 64);
    SafeStr((DWORD)(exeBase + (0x01275DC8 - 0x00400000)), b, 64);
    SafeStr((DWORD)(exeBase + (0x01275F00 - 0x00400000)), c, 64);
    char k[200]; sprintf_s(k, "%s|%s|%s", a, b, c);
    if (strcmp(k, g_guardPrev) != 0) {
        strcpy_s(g_guardPrev, k);
        Log("[GUARDADO] nombre_guardado=\"%s\" nivel_guardado=\"%s\" ultimo_mapa=\"%s\"", a, b, c);
    }
}
static void LogMapa(BYTE* exeBase) {
    DWORD cv = 0, str = 0;
    if (!SafeRead((const DWORD*)(exeBase + (0x01275E90 - 0x00400000)), &cv) || !cv) return;
    if (!SafeRead((const DWORD*)(cv + 4), &str) || !str) return;
    char m[64];
    SafeStr(str, m, (int)sizeof(m));
    if (strcmp(m, g_mapaPrev) != 0) {
        strcpy_s(g_mapaPrev, m);
        Log("[NIVEL] mapa actual = \"%s\"", m);
    }
}

// ---- Hilo propio: vigila los modulos del juego y aplica los parches ----
static DWORD WINAPI InitThread(LPVOID) {
    g_patches[0].desvio = (DWORD)&Detour_ClientConnect;
    g_patches[0].original = (DWORD*)&g_origClientConnect;
    g_patches[1].desvio = (DWORD)&Detour_ClientBegin;
    g_patches[1].original = (DWORD*)&g_origClientBegin;
    g_patches[2].desvio = (DWORD)&Detour_ClientThink;
    g_patches[2].original = (DWORD*)&g_origClientThink;
    g_patches[3].desvio = (DWORD)&Detour_ClientDisc;
    g_patches[3].original = (DWORD*)&g_origClientDisc;
    g_patches[4].desvio = (DWORD)&Detour_ReadPers;
    g_patches[4].original = (DWORD*)&g_origReadPers;
    g_patches[5].desvio = (DWORD)&Detour_WritePers;
    g_patches[5].original = (DWORD*)&g_origWritePers;

    HMODULE exe = GetModuleHandleA(nullptr);
    Log("alice.exe base = %p", (void*)exe);
    ParchearLoadgame((BYTE*)exe);
    Log("Opciones: PARCHE_RUNENTITY=%s PARCHE_ESTADO=%s",
        PARCHE_RUNENTITY ? "si" : "no", PARCHE_ESTADO ? "si" : "no");
    Log("Opciones (alice-coop.ini): SaltarResetClientes=%d ArreglarArbolCliente=%d HardwareWatch=%d HookClientThink=%d HookClientDisconnect=%d DiagIntervaloMs=%d DiagDuracionMs=%d ProtegerClienteMs=%d CopiarEquipoCliente1=%d ProtegerJugador0Ms=%d HookPersistencia=%d RelecturaPersistente=%d SepararSpawn=%d SepararDistancia=%d SeguirJugador0=%d SeguirDistancia=%d SeguirEsperaMs=%d CambiosDuracionMs=%d",
        g_optSaltarReset, g_optArbolCliente, g_optWatchHW, g_optHookThink, g_optHookDisc, g_optDiagMs, g_optDiagDurMs, g_optProtegerMs, g_optEquipo, g_optProt0Ms, g_optPersist, g_optRelectura, g_optSepararSpawn, g_optSepararDist, g_optSeguir, g_optSeguirDist, g_optSeguirMs, g_optCambiosMs);

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
                if (i == 1 && m) {
                    loadTick = GetTickCount(); diagDone = false;
                    memset(g_thinkCount, 0, sizeof(g_thinkCount));
                }
                if (i == 1 && !m) { g_diagHasta = 0; g_protHasta = 0; g_prot0Hasta = 0; }   // los objetos ya no existen
            }
        }
        g_fgameBase = (BYTE*)last[1];
        if (g_optArbolCliente && last[0] && !last[1]) ArreglarArbolCliente((BYTE*)exe);
        if (last[1]) g_arbolLogueado = false;
        if (last[1]) {
            ApplyCodePatch((BYTE*)last[1]);
            ApplyStatePatch((BYTE*)last[1]);
            ApplyStatemapPatch((BYTE*)last[1]);
            ApplyPatches((BYTE*)last[1]);
            if (!diagDone && GetTickCount() - loadTick > 3000) {
                DumpTable((BYTE*)last[1]);
                diagDone = true;
            }
            // Diagnostico periodico tras entrar el cliente 1
            ProteccionRevisar();
            Proteccion0Revisar();
            if (g_diagHasta) {
                DWORD now = GetTickCount();
                if ((int)(now - g_diffProximo) >= 0) { g_diffProximo = now + 250; if ((int)(g_cambiosHasta - now) > 0) { DumpCambios(); EscanearEntidades(); EscanearCampos(); if (g_clasifProximo && (int)(now - g_clasifProximo) >= 0) { g_clasifProximo = 0; DescribirClave(); } } }
                if ((int)(g_diagHasta - now) <= 0) g_diagHasta = 0;
                else if ((int)(now - g_diagProximo) >= 0) {
                    g_diagProximo = now + (DWORD)g_optDiagMs;
                    DumpPlayers("periodico");
                }
            }
        }
        { static DWORD proxMapa = 0; DWORD nm = GetTickCount(); if ((int)(nm - proxMapa) >= 0) { proxMapa = nm + 500; LogMapa((BYTE*)exe); LogGuardado((BYTE*)exe); } }
        Sleep(5);
    }
    return 0;
}

static void LoadOptions(const char* dir) {
    sprintf_s(g_iniPath, "%salice-coop.ini", dir);
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {   // primera vez: crear con los valores por defecto
        WritePrivateProfileStringA("alice-coop", "SaltarResetClientes", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "ArreglarArbolCliente", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "HardwareWatch", "0", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "HookClientThink", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "HookClientDisconnect", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "DiagIntervaloMs", "1000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "DiagDuracionMs", "7200000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "ProtegerClienteMs", "30000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "CopiarEquipoCliente1", "0", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "ProtegerJugador0Ms", "5000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "HookPersistencia", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "RelecturaPersistente", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SepararSpawn", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SepararDistancia", "64", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SeguirJugador0", "0", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SeguirDistancia", "1500", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SeguirEsperaMs", "3000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "CambiosDuracionMs", "120000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "EscanerEntidades", "1", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "ExperimentoEntidad", "0", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "ExperimentoMs", "15000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SaltarCinematicaMapas", g_optSkipMapas, g_iniPath);
        WritePrivateProfileStringA("alice-coop", "SaltarCinematicaMs", "35000", g_iniPath);
        WritePrivateProfileStringA("alice-coop", "PermitirCargarConDosJugadores", "1", g_iniPath);
    }
    g_optSaltarReset = (int)GetPrivateProfileIntA("alice-coop", "SaltarResetClientes", 1, g_iniPath);
    g_optArbolCliente = (int)GetPrivateProfileIntA("alice-coop", "ArreglarArbolCliente", 1, g_iniPath);
    g_optWatchHW = (int)GetPrivateProfileIntA("alice-coop", "HardwareWatch", 0, g_iniPath);
    g_optHookThink = (int)GetPrivateProfileIntA("alice-coop", "HookClientThink", 1, g_iniPath);
    g_optHookDisc = (int)GetPrivateProfileIntA("alice-coop", "HookClientDisconnect", 1, g_iniPath);
    g_optDiagMs = (int)GetPrivateProfileIntA("alice-coop", "DiagIntervaloMs", 1000, g_iniPath);
    g_optDiagDurMs = (int)GetPrivateProfileIntA("alice-coop", "DiagDuracionMs", 7200000, g_iniPath);
    g_optProtegerMs = (int)GetPrivateProfileIntA("alice-coop", "ProtegerClienteMs", 30000, g_iniPath);
    g_optEquipo = (int)GetPrivateProfileIntA("alice-coop", "CopiarEquipoCliente1", 0, g_iniPath);
    g_optProt0Ms = (int)GetPrivateProfileIntA("alice-coop", "ProtegerJugador0Ms", 5000, g_iniPath);
    g_optPersist = (int)GetPrivateProfileIntA("alice-coop", "HookPersistencia", 1, g_iniPath);
    g_optRelectura = (int)GetPrivateProfileIntA("alice-coop", "RelecturaPersistente", 1, g_iniPath);
    g_optSepararSpawn = (int)GetPrivateProfileIntA("alice-coop", "SepararSpawn", 1, g_iniPath);
    g_optSepararDist = (int)GetPrivateProfileIntA("alice-coop", "SepararDistancia", 64, g_iniPath);
    g_optSeguir = (int)GetPrivateProfileIntA("alice-coop", "SeguirJugador0", 0, g_iniPath);
    g_optSeguirDist = (int)GetPrivateProfileIntA("alice-coop", "SeguirDistancia", 1500, g_iniPath);
    g_optSeguirMs = (int)GetPrivateProfileIntA("alice-coop", "SeguirEsperaMs", 3000, g_iniPath);
    g_optCambiosMs = (int)GetPrivateProfileIntA("alice-coop", "CambiosDuracionMs", 120000, g_iniPath);
    g_optEscaner = (int)GetPrivateProfileIntA("alice-coop", "EscanerEntidades", 1, g_iniPath);
    g_optExpEnt = (int)GetPrivateProfileIntA("alice-coop", "ExperimentoEntidad", 0, g_iniPath);
    g_optExpMs = (int)GetPrivateProfileIntA("alice-coop", "ExperimentoMs", 15000, g_iniPath);
    { char tmp[256]; GetPrivateProfileStringA("alice-coop", "SaltarCinematicaMapas", "fortress1$fortress1_start1", tmp, sizeof(tmp), g_iniPath); strcpy_s(g_optSkipMapas, tmp); }
    g_optSkipMs = (int)GetPrivateProfileIntA("alice-coop", "SaltarCinematicaMs", 35000, g_iniPath);
    g_optPermitirCargar = (int)GetPrivateProfileIntA("alice-coop", "PermitirCargarConDosJugadores", 1, g_iniPath);
    if (g_optDiagMs < 100) g_optDiagMs = 100;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
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
        LoadOptions(dir);
        Log("=== alice-coop proxy v0.25 (v0.24 + permitir loadgame con sv_maxclients 2) iniciado (PID %lu) ===", GetCurrentProcessId());

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
    if (reason == DLL_PROCESS_DETACH && g_log) {
        fprintf(g_log, "[SALIDA] el proceso termina (DLL_PROCESS_DETACH, %s)\n", lpReserved ? "cierre del proceso" : "descarga de la DLL");
        fflush(g_log);
    }
    return TRUE;
}