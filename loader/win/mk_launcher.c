// MarbleKnightsLauncher.exe: the release's front door. A release has no game data; here the player
// picks their own IPA once and the launcher runs tools/prepare_win.py (on the bundled Python) to make
// MarbleKnights_Data from it, then starts MarbleKnights.exe with the chosen options.
//
// Folder layout (next to this exe): MarbleKnights.exe, UnityPlayer.dll, GameAssembly.dll,
// python/python.exe (embedded Python), tools/*.py, bin/mk_native.dll (PVRTC decoder and LZ4).
// Settings live in MarbleKnightsLauncher.ini next to the exe.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { ID_IPA = 100, ID_BROWSE, ID_PREPARE, ID_PROGRESS, ID_STAGE, ID_LOG, ID_STATUS, ID_FULLSCREEN, ID_NOGC,
       ID_FOLDER, ID_PLAY, ID_TITLE, ID_GROUP_DATA, ID_GROUP_OPT, ID_IPA_LABEL };
#define WM_PREP_LINE (WM_APP + 1)
#define WM_PREP_EXIT (WM_APP + 2)

static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH];
static HWND g_wnd;
static HFONT g_font, g_title_font;
static HANDLE g_proc;
static int g_zh, g_busy, g_dpi = 96, g_stage, g_stages = 1;

// UI text: Traditional Chinese when Windows is in Chinese, English otherwise
static const wchar_t *T(const wchar_t *en, const wchar_t *zh) { return g_zh ? zh : en; }
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void path_in(wchar_t *out, const wchar_t *rel) { swprintf(out, MAX_PATH, L"%ls\\%ls", g_dir, rel); }
static int exists(const wchar_t *rel) {
    wchar_t p[MAX_PATH];
    path_in(p, rel);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

// game data is ready when prepare_win.py finished its last stage
static int data_ready(void) {
    wchar_t p[MAX_PATH];
    path_in(p, L"MarbleKnights_Data\\mk\\prepare.json");
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;
    char buf[4096] = { 0 };
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return strstr(buf, "\"icon\"") != NULL && exists(L"MarbleKnights_Data\\mk\\image.bin");
}

static void log_line(const wchar_t *s) {
    HWND e = GetDlgItem(g_wnd, ID_LOG);
    int n = GetWindowTextLengthW(e);
    if (n > 60000) { SendMessageW(e, EM_SETSEL, 0, 20000); SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(e); }
    SendMessageW(e, EM_SETSEL, n, n);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void update_state(void) {
    int ready = data_ready();
    SetDlgItemTextW(g_wnd, ID_STATUS, g_busy ? T(L"Preparing the game data...", L"正在準備遊戲資料…")
                    : ready ? T(L"Game data: ready", L"遊戲資料：已準備好")
                    : T(L"Game data: not prepared yet. Choose your Marble Knights IPA (version 2.3.1) and press Prepare.",
                        L"遊戲資料：還沒準備。選擇你的 Marble Knights IPA（2.3.1 版），再按「準備」。"));
    EnableWindow(GetDlgItem(g_wnd, ID_PLAY), ready && !g_busy);
    EnableWindow(GetDlgItem(g_wnd, ID_PREPARE), !g_busy && GetWindowTextLengthW(GetDlgItem(g_wnd, ID_IPA)) > 0);
    EnableWindow(GetDlgItem(g_wnd, ID_BROWSE), !g_busy);
    EnableWindow(GetDlgItem(g_wnd, ID_IPA), !g_busy);
    SetDlgItemTextW(g_wnd, ID_PREPARE, ready ? T(L"Prepare again", L"重新準備") : T(L"Prepare", L"準備"));
}

static void save_settings(void) {
    wchar_t ipa[MAX_PATH];
    GetDlgItemTextW(g_wnd, ID_IPA, ipa, MAX_PATH);
    WritePrivateProfileStringW(L"launcher", L"ipa", ipa, g_ini);
    WritePrivateProfileStringW(L"launcher", L"fullscreen", IsDlgButtonChecked(g_wnd, ID_FULLSCREEN) ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"test", L"nogc", IsDlgButtonChecked(g_wnd, ID_NOGC) ? L"1" : L"0", g_ini);
}

// ---- running prepare_win.py ---------------------------------------------------------------------
typedef struct { HANDLE out; } reader_args;

static DWORD WINAPI reader(LPVOID arg) {
    HANDLE h = ((reader_args *)arg)->out;
    free(arg);
    char buf[4096], line[8192];
    size_t n = 0;
    DWORD got;
    while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || n == sizeof(line) - 1) {
                if (n && line[n - 1] == '\r') n--;
                line[n] = 0;
                int w = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
                wchar_t *s = malloc(w * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, line, -1, s, w);
                PostMessageW(g_wnd, WM_PREP_LINE, 0, (LPARAM)s);
                n = 0;
            } else {
                line[n++] = buf[i];
            }
        }
    }
    CloseHandle(h);
    return 0;
}

static DWORD WINAPI waiter(LPVOID arg) {
    WaitForSingleObject((HANDLE)arg, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)arg, &code);
    PostMessageW(g_wnd, WM_PREP_EXIT, code, 0);
    return 0;
}

static void start_prepare(void) {
    wchar_t ipa[MAX_PATH], py[MAX_PATH], script[MAX_PATH], lz4[MAX_PATH], pvrtc[MAX_PATH];
    GetDlgItemTextW(g_wnd, ID_IPA, ipa, MAX_PATH);
    if (GetFileAttributesW(ipa) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_wnd, T(L"The IPA file was not found.", L"找不到這個 IPA 檔案。"), L"Marble Knights", MB_ICONWARNING);
        return;
    }
    path_in(py, L"python\\python.exe");
    if (GetFileAttributesW(py) == INVALID_FILE_ATTRIBUTES) wcscpy(py, L"python.exe");  // a development folder
    path_in(script, L"tools\\prepare_win.py");
    path_in(lz4, L"bin\\mk_native.dll");
    path_in(pvrtc, L"bin\\mk_native.dll");
    static wchar_t cmd[4 * MAX_PATH + 256];
    swprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%ls\" -I -u \"%ls\" \"%ls\" \"%ls\"", py, script, ipa, g_dir);
    if (GetFileAttributesW(lz4) != INVALID_FILE_ATTRIBUTES) swprintf(cmd + wcslen(cmd), 2 * MAX_PATH, L" --lz4 \"%ls\"", lz4);
    if (GetFileAttributesW(pvrtc) != INVALID_FILE_ATTRIBUTES) swprintf(cmd + wcslen(cmd), 2 * MAX_PATH, L" --pvrtc \"%ls\"", pvrtc);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, g_dir, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        MessageBoxW(g_wnd, T(L"Python could not be started (python\\python.exe is missing).", L"無法啟動 Python（缺少 python\\python.exe）。"),
                    L"Marble Knights", MB_ICONERROR);
        return;
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_busy = 1;
    g_stage = 0;
    SetWindowTextW(GetDlgItem(g_wnd, ID_LOG), L"");
    SendDlgItemMessageW(g_wnd, ID_PROGRESS, PBM_SETPOS, 0, 0);
    reader_args *ra = malloc(sizeof(*ra));
    ra->out = rd;
    CloseHandle(CreateThread(NULL, 0, reader, ra, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, waiter, g_proc, 0, NULL));
    save_settings();
    update_state();
}

static const wchar_t *stage_name(const wchar_t *s) {
    static const struct { const wchar_t *key, *en, *zh; } names[] = {
        { L"check", L"Checking the IPA", L"檢查 IPA" },
        { L"extract", L"Extracting the game data", L"解出遊戲資料" },
        { L"player data", L"Converting the player data", L"轉換主資料" },
        { L"asset bundles", L"Converting shaders and textures (the longest step)", L"轉換著色器與貼圖（最久的一步）" },
        { L"image", L"Preparing the game code's data", L"準備遊戲程式的資料" },
        { L"icon", L"Setting the icon", L"設定圖示" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!wcscmp(s, names[i].key)) return T(names[i].en, names[i].zh);
    return s;
}

static void on_line(wchar_t *s) {
    if (!wcsncmp(s, L"@progress ", 10)) {
        double done = 0, total = 1;
        swscanf(s + 10, L"%lf %lf", &done, &total);
        double f = total > 0 ? done / total : 0;
        int pos = (int)(1000.0 * ((g_stage - 1) + f) / g_stages);
        SendDlgItemMessageW(g_wnd, ID_PROGRESS, PBM_SETPOS, pos, 0);
    } else if (!wcsncmp(s, L"@stage ", 7)) {
        int k = 0, n = 1, used = 0;
        swscanf(s + 7, L"%d %d %n", &k, &n, &used);
        g_stage = k;
        g_stages = n > 0 ? n : 1;
        wchar_t text[256];
        swprintf(text, 256, L"%d/%d  %ls", k, n, stage_name(s + 7 + used));
        SetDlgItemTextW(g_wnd, ID_STAGE, text);
        log_line(text);
        SendDlgItemMessageW(g_wnd, ID_PROGRESS, PBM_SETPOS, (int)(1000.0 * (k - 1) / g_stages), 0);
    } else if (!wcscmp(s, L"@done")) {
        SendDlgItemMessageW(g_wnd, ID_PROGRESS, PBM_SETPOS, 1000, 0);
        SetDlgItemTextW(g_wnd, ID_STAGE, T(L"Done", L"完成"));
    } else if (!wcsncmp(s, L"@error ", 7)) {
        log_line(s + 7);
        MessageBoxW(g_wnd, s + 7, T(L"Preparing failed", L"準備失敗"), MB_ICONERROR);
    } else {
        log_line(s);
    }
    free(s);
}

// ---- playing ------------------------------------------------------------------------------------
static void play(void) {
    save_settings();
    int full = IsDlgButtonChecked(g_wnd, ID_FULLSCREEN) == BST_CHECKED;
    SetEnvironmentVariableW(L"MK_FULLSCREEN", full ? L"1" : NULL);
    SetEnvironmentVariableW(L"MK_NOGC", IsDlgButtonChecked(g_wnd, ID_NOGC) == BST_CHECKED ? L"1" : NULL);
    wchar_t exe[MAX_PATH], cmd[2 * MAX_PATH];
    path_in(exe, L"MarbleKnights.exe");
    // the player starts windowed: full screen is our own borderless window (GameAssembly.dll)
    swprintf(cmd, 2 * MAX_PATH, L"\"%ls\" -screen-fullscreen 0 -screen-width 1280 -screen-height 720", exe);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        MessageBoxW(g_wnd, T(L"MarbleKnights.exe could not be started.", L"無法啟動 MarbleKnights.exe。"), L"Marble Knights", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g_wnd);
}

static void browse(void) {
    wchar_t file[MAX_PATH] = L"";
    GetDlgItemTextW(g_wnd, ID_IPA, file, MAX_PATH);
    OPENFILENAMEW of = { sizeof(of) };
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"IPA (*.ipa)\0*.ipa\0*.*\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) {
        SetDlgItemTextW(g_wnd, ID_IPA, file);
        update_state();
    }
}

// ---- window -------------------------------------------------------------------------------------
static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(id == ID_TITLE ? g_title_font : g_font), TRUE);
    return c;
}

static void make_fonts(void) {
    if (g_font) DeleteObject(g_font);
    if (g_title_font) DeleteObject(g_title_font);
    NONCLIENTMETRICSW m = { sizeof(m) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
    m.lfMessageFont.lfHeight = -MulDiv(9, g_dpi, 72);
    g_font = CreateFontIndirectW(&m.lfMessageFont);
    m.lfMessageFont.lfHeight = -MulDiv(14, g_dpi, 72);
    m.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g_title_font = CreateFontIndirectW(&m.lfMessageFont);
}

static void build_controls(void) {
    wchar_t ipa[MAX_PATH];
    GetPrivateProfileStringW(L"launcher", L"ipa", L"", ipa, MAX_PATH, g_ini);
    add(L"STATIC", L"Marble Knights Recompiled", 0, 16, 12, 520, 28, ID_TITLE);
    add(L"BUTTON", T(L"Game data", L"遊戲資料"), BS_GROUPBOX, 12, 46, 536, 250, ID_GROUP_DATA);
    add(L"STATIC", L"", 0, 24, 68, 512, 34, ID_STATUS);
    add(L"STATIC", L"IPA", 0, 24, 108, 30, 20, ID_IPA_LABEL);
    add(L"EDIT", ipa, ES_AUTOHSCROLL | WS_TABSTOP, 56, 105, 380, 24, ID_IPA);
    add(L"BUTTON", T(L"Browse...", L"瀏覽…"), BS_PUSHBUTTON | WS_TABSTOP, 444, 104, 92, 26, ID_BROWSE);
    add(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, 24, 138, 120, 28, ID_PREPARE);
    add(L"STATIC", L"", SS_LEFTNOWORDWRAP, 154, 144, 382, 20, ID_STAGE);
    HWND pb = add(PROGRESS_CLASSW, L"", 0, 24, 174, 512, 16, ID_PROGRESS);
    SendMessageW(pb, PBM_SETRANGE32, 0, 1000);
    add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 24, 198, 512, 88, ID_LOG);
    add(L"BUTTON", T(L"Options", L"選項"), BS_GROUPBOX, 12, 304, 536, 78, ID_GROUP_OPT);
    add(L"BUTTON", T(L"Start in full screen (Alt+Enter or F4 switches in the game)", L"以全螢幕開始（遊戲中按 Alt+Enter 或 F4 切換）"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 24, 324, 512, 22, ID_FULLSCREEN);
    add(L"BUTTON", T(L"Test: turn off the garbage collector (for bug reports)", L"測試：關閉記憶體回收（回報問題用）"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 24, 350, 512, 22, ID_NOGC);
    CheckDlgButton(g_wnd, ID_FULLSCREEN, GetPrivateProfileIntW(L"launcher", L"fullscreen", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_NOGC, GetPrivateProfileIntW(L"test", L"nogc", 0, g_ini) ? BST_CHECKED : BST_UNCHECKED);
    add(L"BUTTON", T(L"Open folder", L"開啟資料夾"), BS_PUSHBUTTON | WS_TABSTOP, 12, 394, 130, 32, ID_FOLDER);
    add(L"BUTTON", T(L"Play", L"開始遊戲"), BS_DEFPUSHBUTTON | WS_TABSTOP, 418, 394, 130, 32, ID_PLAY);
    update_state();
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = w;
        make_fonts();
        build_controls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BROWSE: browse(); break;
        case ID_PREPARE: start_prepare(); break;
        case ID_PLAY: play(); break;
        case ID_FOLDER: ShellExecuteW(w, L"open", g_dir, NULL, NULL, SW_SHOWNORMAL); break;
        case ID_IPA: if (HIWORD(wp) == EN_CHANGE) update_state(); break;
        }
        return 0;
    case WM_PREP_LINE:
        on_line((wchar_t *)lp);
        return 0;
    case WM_PREP_EXIT:
        CloseHandle(g_proc);
        g_proc = NULL;
        g_busy = 0;
        if (wp == 0 && data_ready()) log_line(T(L"The game data is ready.", L"遊戲資料準備好了。"));
        else if (wp) log_line(T(L"Preparing stopped. Press Prepare to continue where it stopped.", L"準備中斷了；再按「準備」會從中斷的地方繼續。"));
        update_state();
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        if (g_busy && MessageBoxW(w, T(L"Stop preparing? It can continue later.", L"要停止準備嗎？之後可以接著做。"), L"Marble Knights",
                                  MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        if (g_proc) TerminateProcess(g_proc, 1);
        save_settings();
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev;
    (void)cmdline;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;
    swprintf(g_ini, MAX_PATH, L"%ls\\MarbleKnightsLauncher.ini", g_dir);
    g_zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"MarbleKnightsLauncher";
    RegisterClassW(&wc);
    RECT r = { 0, 0, S(560), S(438) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowW(wc.lpszClassName, T(L"Marble Knights Launcher", L"Marble Knights 啟動器"), style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
