// MarbleKnights.exe for the Windows build: Unity's WindowsPlayer.exe with -force-gles30 always on.
// The data only has GLES3 shaders (converted from the iOS Metal ones); without the switch the
// player picks Direct3D 11 and everything renders with the pink error shader.
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

typedef int (*unity_main_fn)(HINSTANCE, HINSTANCE, LPWSTR, int);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    HMODULE up = LoadLibraryW(L"UnityPlayer.dll");
    unity_main_fn unity_main = up ? (unity_main_fn)GetProcAddress(up, "UnityMain") : NULL;
    if (!unity_main) {
        MessageBoxW(NULL, L"UnityPlayer.dll not found", L"Marble Knights", MB_ICONERROR);
        return 1;
    }
    static wchar_t line[32768];
    if (wcsstr(cmd, L"-force-")) swprintf(line, 32768, L"%ls", cmd);  // a renderer chosen by hand wins
    else swprintf(line, 32768, L"-force-gles30 %ls", cmd);
    SetEnvironmentVariableW(L"MK_GLES", L"1");  // GameAssembly.dll's renderer check (the process command line is unchanged)
    return unity_main(inst, prev, line, show);
}
