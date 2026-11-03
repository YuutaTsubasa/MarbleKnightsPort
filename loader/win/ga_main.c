// GameAssembly.dll for the Windows test build (milestone 1): log every il2cpp API call the
// official UnityPlayer.dll makes, in order, to mk_ga.log next to the exe.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <windows.h>

static FILE *g_log;
static CRITICAL_SECTION g_mu;

void mk_ga_log_call(const char *name, int *seen) {
    EnterCriticalSection(&g_mu);
    if (!*seen || *seen < 3) {
        if (g_log) { fprintf(g_log, "%lu %s\n", GetCurrentThreadId(), name); fflush(g_log); }
    }
    (*seen)++;
    LeaveCriticalSection(&g_mu);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        InitializeCriticalSection(&g_mu);
        GetModuleFileNameA(inst, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "mk_ga.log");
        g_log = fopen(path, "w");
        if (g_log) { fprintf(g_log, "GameAssembly.dll loaded\n"); fflush(g_log); }
    }
    return TRUE;
}
