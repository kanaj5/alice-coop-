// dllmain.cpp : Define el punto de entrada de la aplicación DLL.
#include "pch.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

static HMODULE g_real = nullptr;
static FILE* g_log = nullptr;

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
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args; va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fprintf(g_log, "\n");
    fflush(g_log);
}

// ---- Funciones que exportamos: solo reenvían a la DLL real ----
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

// ---- Hilo propio: observa cuándo se cargan los módulos del juego ----
static DWORD WINAPI InitThread(LPVOID) {
    HMODULE exe = GetModuleHandleA(nullptr);
    const char* mz = reinterpret_cast<const char*>(exe);
    Log("alice.exe base = %p", (void*)exe);
    Log("Primeros bytes del exe: %c%c (deben ser MZ)", mz[0], mz[1]);

    const char* names[] = { "cgamex86.dll", "fgamex86.dll" };
    HMODULE last[2] = { nullptr, nullptr };
    for (;;) {
        for (int i = 0; i < 2; i++) {
            HMODULE m = GetModuleHandleA(names[i]);
            if (m != last[i]) {
                if (m) Log("%s CARGADO en %p", names[i], (void*)m);
                else   Log("%s DESCARGADO", names[i]);
                last[i] = m;
            }
        }
        Sleep(250);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        // El log se guarda junto a nuestra DLL
        char dir[MAX_PATH];
        GetModuleFileNameA(hModule, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\');
        if (slash) *(slash + 1) = '\0';
        char logPath[MAX_PATH];
        sprintf_s(logPath, "%salice-coop.log", dir);
        fopen_s(&g_log, logPath, "w");
        Log("=== alice-coop proxy iniciado (PID %lu) ===", GetCurrentProcessId());

        // Cargar la xinput1_3.dll REAL del sistema (SysWOW64 en un proceso de 32 bits)
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
