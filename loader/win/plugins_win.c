// Windows replacements for the game's iOS native plugins (the same ones loader/proxy replaces on
// Android): CloudKit saves as local files, a signed-in Game Center player, AppleGameController
// answered from XInput and the keyboard, and no-op haptics / compass. Each replaces a translated
// UnityFramework function (offsets in UnityFramework 2.3.1, as in the Android proxy), so it takes
// the guest registers: arguments in X(0..7), results in x0 (or v0 / the x8 buffer, as Apple returns them).
// Managed callbacks the plugins received are guest addresses, called with mk_guest_enter.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <direct.h>

#include "win_rt.h"

void mk_override_function(uint64_t off, void (*with)(cpu *), const char *what);  // win_rt.c

#define P(i) ((const char *)(uintptr_t)X(i))
static DWORD g_main_tid;
static char g_save_dir[MAX_PATH];  // <game folder>\save

static void call0(uint64_t fn) { if (fn) mk_guest_enter(fn, NULL, 0, NULL, 0, NULL); }
static void calln(uint64_t fn, const uint64_t *x, int n) { if (fn) mk_guest_enter(fn, x, n, NULL, 0, NULL); }
static double now_s(void) { return GetTickCount64() / 1000.0; }

// ---- CloudKit: "iCloud" records as files in save\icloud ----
// callback table order follows the C# EventHandlers struct (see loader/proxy/proxy.c)
static struct { uint64_t account_status_changed, fetch_succeeded, fetch_failed, save_succeeded, save_requires_merge, save_failed; } g_ck;
typedef struct ck_event { int kind; char *record; void *data; int len; double due; struct ck_event *next; } ck_event;
static ck_event *g_ck_head, **g_ck_tail = &g_ck_head;
static SRWLOCK g_ck_lock = SRWLOCK_INIT;

static void ck_path(const char *record, char *out, size_t n) {
    char safe[256];
    size_t k = 0;
    for (; record && record[k] && k < sizeof(safe) - 1; k++)
        safe[k] = strchr("/\\:*?\"<>|", record[k]) ? '_' : record[k];
    safe[k] = 0;
    snprintf(out, n, "%s\\icloud\\%s.bin", g_save_dir, safe);
}
// Real CloudKit answers asynchronously and the game registers its completion handler only after
// FetchRecord / SaveRecord return, so results are queued and delivered later from the main thread.
static void ck_post(int kind, const char *record, void *data, int len) {
    ck_event *e = calloc(1, sizeof(*e));
    e->kind = kind; e->record = _strdup(record ? record : ""); e->data = data; e->len = len;
    e->due = now_s() + 1.5;
    AcquireSRWLockExclusive(&g_ck_lock);
    *g_ck_tail = e; g_ck_tail = &e->next;
    ReleaseSRWLockExclusive(&g_ck_lock);
}
static void ck_pump(void) {
    for (;;) {
        AcquireSRWLockExclusive(&g_ck_lock);
        ck_event *e = g_ck_head;
        if (e && e->due > now_s()) e = NULL;
        else if (e) { g_ck_head = e->next; if (!g_ck_head) g_ck_tail = &g_ck_head; }
        ReleaseSRWLockExclusive(&g_ck_lock);
        if (!e) break;
        uint64_t rec = (uint64_t)(uintptr_t)e->record, tag = (uint64_t)(uintptr_t)"local";
        switch (e->kind) {
        case 0: { uint64_t a[4] = { (uint64_t)(uintptr_t)e->data, (uint64_t)e->len, rec, tag }; calln(g_ck.fetch_succeeded, a, 4); break; }
        case 1: { uint64_t a[4] = { rec, 11 /* CKErrorUnknownItem */, (uint64_t)(uintptr_t)"Record not found", rec }; calln(g_ck.fetch_failed, a, 4); break; }
        case 2: { uint64_t a[2] = { rec, tag }; calln(g_ck.save_succeeded, a, 2); break; }
        case 3: { uint64_t a[4] = { rec, 1, (uint64_t)(uintptr_t)"local write failed", rec }; calln(g_ck.save_failed, a, 4); break; }
        }
        free(e->data); free(e->record); free(e);
    }
}
static void ck_initialize(cpu *c) {
    memcpy(&g_ck, (const void *)(uintptr_t)X(0), sizeof(g_ck));
    g_main_tid = GetCurrentThreadId();
    char dir[MAX_PATH + 16];
    snprintf(dir, sizeof(dir), "%s\\icloud", g_save_dir);
    _mkdir(g_save_dir);
    _mkdir(dir);
    mk_log("CloudKit(local) initialize container=%s -> %s", X(1) ? P(1) : "(null)", dir);
    call0(g_ck.account_status_changed);
}
static void ck_account_status(cpu *c) { RET(3); }  // CKAccountStatusNoAccount: the game keeps saves locally
static void ck_get_auth_error(cpu *c) { RET(mk_strdup("")); }
static void ck_noop(cpu *c) { (void)c; }
static void ck_fetch_record(cpu *c) {
    const char *record = P(0);
    char path[MAX_PATH + 300];
    ck_path(record, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { mk_log("CloudKit(local) fetch %s: no record", record); ck_post(1, record, NULL, 0); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? n : 1);
    n = (long)fread(buf, 1, n, f);
    fclose(f);
    mk_log("CloudKit(local) fetch %s: %ld bytes", record, n);
    ck_post(0, record, buf, (int)n);
}
static void ck_save_record(cpu *c) {
    const void *data = (const void *)(uintptr_t)X(0);
    int len = (int)X(1);
    const char *record = P(2);
    char path[MAX_PATH + 300], tmp[MAX_PATH + 320];
    ck_path(record, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, (size_t)len, f) == (size_t)len;
    if (f) fclose(f);
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
    mk_log("CloudKit(local) save %s: %d bytes %s", record, len, ok ? "ok" : "FAILED");
    ck_post(ok ? 2 : 3, record, NULL, 0);
}
static void ck_show_alert(cpu *c) { mk_log("CloudKit(local) alert: %s / %s", X(0) ? P(0) : "", X(1) ? P(1) : ""); }

// ---- Game Center: a signed-in local player; no matches on Windows yet ----
static void gc_initialize(cpu *c) {
    uint64_t on_auth = ((const uint64_t *)(uintptr_t)X(0))[0];  // OnAuthentication(bool ok, const char *error)
    mk_log("GameCenter(local) initialize: reporting an authenticated player");
    uint64_t a[2] = { 1, 0 };
    calln(on_auth, a, 2);
}
static void gc_true(cpu *c) { RET(1); }
static void gc_false(cpu *c) { RET(0); }
static void gc_minus1(cpu *c) { RET(-1); }
static void gc_local_player(cpu *c) {  // struct { const char *id, *name; } in x0 / x1
    c->x[0] = (uint64_t)(uintptr_t)mk_strdup("G:0000000001");
    c->x[1] = (uint64_t)(uintptr_t)mk_strdup("Player");
}

// ---- compass (MagneticHeading): none ----
static void mh_float_m1(cpu *c) { c->v[0].u[0] = 0; c->v[0].u[1] = 0; c->v[0].f[0] = -1.0f; }
static void mh_float_0(cpu *c) { c->v[0].u[0] = 0; c->v[0].u[1] = 0; }
static void mh_vector(cpu *c) { for (int i = 0; i < 3; i++) { c->v[i].u[0] = 0; c->v[i].u[1] = 0; } }  // HFA in s0-s2

// ---- AppleGameController: XInput pads; the keyboard drives pad 0 too ----
typedef struct {  // AppleGameController.AppleController (blittable, 40 bytes)
    uint8_t is_valid, is_micro, pad_[2];
    float lx, ly, rx, ry, dx, dy;
    uint8_t any, a, b, x, y, lb, lt, rb, rt, home, menu, options;
} apple_controller;
_Static_assert(sizeof(apple_controller) == 40, "AppleController");

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xi_gamepad;
typedef struct { DWORD packet; xi_gamepad pad; } xi_state;
static DWORD (WINAPI *g_xinput_get)(DWORD, xi_state *);
static uint64_t g_agc_connected, g_agc_disconnected;
static int g_agc_ready, g_agc_known, g_agc_slot[4];

static int own_window_focused(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static float stick(SHORT v) { float f = v / 32767.0f; if (f < -1) f = -1; return (f > -0.24f && f < 0.24f) ? 0 : f; }

// slots = XInput users 0-3 that are connected. The keyboard is not a pad here: Rewired reads it
// itself, and an always-connected pad kept the game in controller mode (no touch controls).
static unsigned agc_slots(void) {
    unsigned m = 0;
    xi_state s;
    for (DWORD u = 0; g_xinput_get && u < 4; u++) if (g_xinput_get(u, &s) == 0) m |= 1u << u;
    return m;
}
static apple_controller pad_state(int slot) {
    apple_controller p;
    memset(&p, 0, sizeof(p));
    p.is_valid = 1;
    xi_state s;
    if (g_xinput_get && g_xinput_get((DWORD)slot, &s) == 0) {
        WORD b = s.pad.buttons;
        p.lx = stick(s.pad.lx); p.ly = stick(s.pad.ly); p.rx = stick(s.pad.rx); p.ry = stick(s.pad.ry);
        p.dx = (b & 0x8 ? 1.0f : 0) - (b & 0x4 ? 1.0f : 0);
        p.dy = (b & 0x1 ? 1.0f : 0) - (b & 0x2 ? 1.0f : 0);
        p.a = !!(b & 0x1000); p.b = !!(b & 0x2000); p.x = !!(b & 0x4000); p.y = !!(b & 0x8000);
        p.lb = !!(b & 0x100); p.rb = !!(b & 0x200); p.lt = s.pad.lt > 128; p.rt = s.pad.rt > 128;
        p.menu = !!(b & 0x10); p.options = !!(b & 0x20);
    }
    p.any = p.a | p.b | p.x | p.y | p.lb | p.rb | p.lt | p.rt | p.menu | p.options;
    return p;
}
static void agc_initialize(cpu *c) {
    const uint64_t *h = (const uint64_t *)(uintptr_t)X(0);
    g_agc_connected = h[0];
    g_agc_disconnected = h[1];
    g_main_tid = GetCurrentThreadId();
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    g_xinput_get = x ? (DWORD (WINAPI *)(DWORD, xi_state *))GetProcAddress(x, "XInputGetState") : NULL;
    g_agc_ready = 1;
    mk_log("AppleGameController(windows) initialize: XInput %s", g_xinput_get ? "found" : "missing");
}
static void agc_noop(cpu *c) { (void)c; }
static void agc_count(cpu *c) { RET(g_agc_known); }
static void agc_vendor(cpu *c) { RET(mk_strdup("Xbox Wireless Controller")); }
static void agc_glyph(cpu *c) { RET(mk_strdup("")); }  // the game falls back to its generic icons
static void agc_get(cpu *c) {  // 40-byte struct: Apple returns it through the x8 buffer
    int i = (int)X(0);
    apple_controller p;
    if (i >= 0 && i < g_agc_known) p = pad_state(g_agc_slot[i]);
    else memset(&p, 0, sizeof(p));
    static unsigned calls, last[4];
    unsigned bits = (unsigned)p.a | p.b << 1 | p.x << 2 | p.y << 3 | p.lb << 4 | p.rb << 5 | p.lt << 6 | p.rt << 7 | p.menu << 8 |
                    p.options << 9 | (p.lx > 0.5f) << 10 | (p.lx < -0.5f) << 11 | (p.ly > 0.5f) << 12 | (p.ly < -0.5f) << 13;
    if (!calls++) mk_log("AGCGetController first call (index %d)", i);
    if (i >= 0 && i < 4 && bits != last[i]) { last[i] = bits; mk_log("pad %d state %04x", i, bits); }
    memcpy((void *)(uintptr_t)c->x[8], &p, sizeof(p));
}
// The game's controller index follows Apple's GCController.controllers: a compact list in connection
// order; g_agc_slot maps it to our slots (like the Android proxy).
static void agc_pump(void) {
    static unsigned tick;
    if (!g_agc_ready || (tick++ % 64)) return;
    unsigned slots = agc_slots(), known = 0;
    for (int i = 0; i < g_agc_known;) {
        if (slots & (1u << g_agc_slot[i])) { known |= 1u << g_agc_slot[i]; i++; continue; }
        mk_log("gamepad %d (slot %d) disconnected", i, g_agc_slot[i]);
        memmove(&g_agc_slot[i], &g_agc_slot[i + 1], (size_t)(g_agc_known - i - 1) * sizeof(int));
        g_agc_known--;
        uint64_t a[3] = { 0, 1, (uint64_t)i };
        calln(g_agc_disconnected, a, 3);
    }
    for (int s = 0; s < 4 && g_agc_known < 4; s++) {
        if (!(slots & ~known & (1u << s))) continue;
        int i = g_agc_known++;
        g_agc_slot[i] = s;
        mk_log("gamepad %d (slot %d) connected", i, s);
        uint64_t a[3] = { 0, 1, (uint64_t)i };
        calln(g_agc_connected, a, 3);
    }
}

// MK_DUMP_RENDER=1: every few seconds log the scene's lighting and fog settings (engine icalls)
void *mk_find_icall(const char *name);  // win_rt.c
typedef struct { float r, g, b, a; } color4;
static void dump_render(void) {
    static int on = -1;
    static ULONGLONG next;
    if (on < 0) { char v[4]; on = GetEnvironmentVariableA("MK_DUMP_RENDER", v, sizeof(v)) > 0; }
    if (!on || GetTickCount64() < next) return;
    next = GetTickCount64() + 5000;
#define ICALL(ty, name, ...) ((ty (*)(__VA_ARGS__))mk_find_icall(name))
    int (*geti)(void);
    float (*getf)(void);
    void (*getc)(color4 *);
    color4 amb = { 0 }, fogc = { 0 }, lc = { 0 };
    int fog = (geti = ICALL(int, "UnityEngine.RenderSettings::get_fog", void)) ? geti() & 0xff : -1;
    int fmode = (geti = ICALL(int, "UnityEngine.RenderSettings::get_fogMode", void)) ? geti() : -1;
    float fd = (getf = ICALL(float, "UnityEngine.RenderSettings::get_fogDensity", void)) ? getf() : -1;
    float fs = (getf = ICALL(float, "UnityEngine.RenderSettings::get_fogStartDistance", void)) ? getf() : -1;
    float fe = (getf = ICALL(float, "UnityEngine.RenderSettings::get_fogEndDistance", void)) ? getf() : -1;
    if ((getc = ICALL(void, "UnityEngine.RenderSettings::get_fogColor_Injected", color4 *))) getc(&fogc);
    int amode = (geti = ICALL(int, "UnityEngine.RenderSettings::get_ambientMode", void)) ? geti() : -1;
    float ai = (getf = ICALL(float, "UnityEngine.RenderSettings::get_ambientIntensity", void)) ? getf() : -1;
    if ((getc = ICALL(void, "UnityEngine.RenderSettings::get_ambientLight_Injected", color4 *))) getc(&amb);
    void *(*get_sun)(void) = ICALL(void *, "UnityEngine.RenderSettings::get_sun", void);
    void *sun = get_sun ? get_sun() : NULL;
    float li = -1;
    if (sun) {
        float (*inten)(void *) = ICALL(float, "UnityEngine.Light::get_intensity", void *);
        void (*col)(void *, color4 *) = ICALL(void, "UnityEngine.Light::get_color_Injected", void *, color4 *);
        if (inten) li = inten(sun);
        if (col) col(sun, &lc);
    }
    mk_log("render: fog=%d mode=%d density=%g start=%g end=%g color=(%.2f %.2f %.2f) ambient mode=%d intensity=%g light=(%.2f %.2f %.2f) "
           "sun=%p intensity=%g color=(%.2f %.2f %.2f)", fog, fmode, fd, fs, fe, fogc.r, fogc.g, fogc.b, amode, ai, amb.r, amb.g, amb.b,
           sun, li, lc.r, lc.g, lc.b);
#undef ICALL
}

// Alt+Enter / F4: switch between full screen (the monitor's size) and a 1280x720 window
static void fullscreen_hotkey(void) {
    static int was;
    int now = own_window_focused() && ((key(VK_MENU) && key(VK_RETURN)) || (key(VK_F4) && !key(VK_MENU)));
    if (now && !was) {
        uint8_t (*is_full)(void) = (uint8_t (*)(void))mk_find_icall("UnityEngine.Screen::get_fullScreen");
        void (*set_res)(int32_t, int32_t, int32_t, int32_t) =
            (void (*)(int32_t, int32_t, int32_t, int32_t))mk_find_icall("UnityEngine.Screen::SetResolution");
        if (is_full && set_res) {
            int full = is_full() & 0xff;
            HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoA(mon, &mi);
            int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (full) set_res(1280, 720, 3 /* Windowed */, 0);
            else set_res(mw, mh, 1 /* FullScreenWindow */, 0);
            mk_log("display: %s", full ? "windowed 1280x720" : "full screen");
        }
    }
    was = now;
}

// main thread, from the engine's managed calls (il2cpp_runtime_invoke)
void mk_plugins_pump(void) {
    static int busy;
    if (busy || !g_main_tid || GetCurrentThreadId() != g_main_tid) return;
    busy = 1;
    if (g_ck_head) ck_pump();
    agc_pump();
    fullscreen_hotkey();
    dump_render();
    busy = 0;
}

void mk_plugins_install(void) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(g_save_dir, sizeof(g_save_dir), "%s\\save", exe);
    static const struct { uint64_t off; void (*fn)(cpu *); const char *name; } fns[] = {
        { 0x1fea9c, ck_initialize, "_CKInitialize" }, { 0x1fea08, ck_get_auth_error, "_GetAuthError" },
        { 0x1feb38, ck_noop, "_CKShutdown" }, { 0x1feb80, ck_account_status, "_CKAccountStatus" },
        { 0x1febd4, ck_fetch_record, "_CKFetchRecord" }, { 0x1fec44, ck_save_record, "_CKSaveRecord" },
        { 0x1fecf4, ck_noop, "_CKReverseSaveRecord" }, { 0x1fed64, ck_show_alert, "_CKShowAlert" },
        { 0x209a68, gc_initialize, "_GCInitialize" }, { 0x209af4, ck_noop, "_GCTimeoutInitialize" },
        { 0x209b3c, ck_noop, "_GCShutdown" }, { 0x209be8, gc_true, "_GCIsAuthenticated" },
        { 0x209ce0, gc_false, "_GCIsUnderage" }, { 0x209f64, gc_false, "_GCCanShowAccessPoint" },
        { 0x20a0c0, gc_false, "_GCIsShowingDashboard" }, { 0x20a020, ck_noop, "_GCShowAccessPoint" },
        { 0x20a070, ck_noop, "_GCSetAccessPointFocus" }, { 0x20a658, ck_noop, "_GCReportAchievement" },
        { 0x209b84, gc_local_player, "_GCLocalPlayer" }, { 0x209c3c, gc_true, "_GCIsMultiplayerRestricted" },
        { 0x209d84, ck_noop, "_GCLoadRecentPlayers" }, { 0x209dcc, gc_false, "_GCGetPlayersInMatch" },
        { 0x209e20, ck_noop, "_GCStartSearchingForNearbyPlayers" }, { 0x209e68, ck_noop, "_GCStopSearchingForNearbyPlayers" },
        { 0x209eb0, ck_noop, "_GCShowActivityViewController" }, { 0x20a114, ck_noop, "_GCDisplayDashboard" },
        { 0x20a15c, ck_noop, "_GCShowMatchmaker" }, { 0x20a1d4, ck_noop, "_GCShowMatchmakerForInvite" },
        { 0x20a21c, ck_noop, "_GCStartMatchmaking" }, { 0x20a294, ck_noop, "_GCStartEmptyMatch" },
        { 0x20a2f4, ck_noop, "_GCAddPlayersToMatch" }, { 0x20a36c, ck_noop, "_GCCancelMatchmaking" },
        { 0x20a3b4, ck_noop, "_GCFinishMatchmaking" }, { 0x20a3fc, ck_noop, "_GCAcceptMatchmakingInvite" },
        { 0x20a444, ck_noop, "_GCDisconnectMatch" }, { 0x20a48c, ck_noop, "_GCSendMatchData" },
        { 0x20a528, ck_noop, "_GCSendMatchDataToPlayers" }, { 0x20a6e8, ck_noop, "_GCLoadAchievements" },
        { 0x20a730, ck_noop, "_GCResetAchievements" }, { 0x20a778, ck_noop, "_GCShowAchievements" },
        { 0x20a7f0, ck_noop, "_GCLoadFriendPlayers" }, { 0x20a838, ck_noop, "_GCCancelInviteForPlayers" },
        { 0x20a914, gc_minus1, "_GCGetStartingRoomNumber" },
        { 0x20901c, ck_noop, "InstantiateFeedbackGenerators" }, { 0x2090f4, ck_noop, "ReleaseFeedbackGenerators" },
        { 0x209150, ck_noop, "SelectionHaptic" }, { 0x209188, ck_noop, "SuccessHaptic" },
        { 0x2091c4, ck_noop, "WarningHaptic" }, { 0x209200, ck_noop, "FailureHaptic" },
        { 0x20923c, ck_noop, "LightImpactHaptic" }, { 0x209274, ck_noop, "MediumImpactHaptic" },
        { 0x2092ac, ck_noop, "HeavyImpactHaptic" },
        { 0x2092e4, ck_noop, "_MHStart" }, { 0x209338, ck_noop, "_MHStop" },
        { 0x209380, gc_false, "_MHHeadingAvailable" }, { 0x2093d4, mh_float_m1, "_MHHeadingAccuracy" },
        { 0x209430, mh_float_0, "_MHGetMagneticHeading" }, { 0x20948c, mh_vector, "_MHGetMagneticVector" },
        { 0x20a9b4, agc_initialize, "_AGCInitialize" }, { 0x20aa04, agc_noop, "_AGCShutdown" },
        { 0x20aa94, agc_noop, "_AGCPrintControllerStatus" }, { 0x20aa4c, agc_noop, "_AGCAdjustControllerPluginData" },
        { 0x20aadc, agc_count, "_AGCGetTotalNumberOfControllers" }, { 0x20ab9c, agc_vendor, "_AGCGetVendorNameForController" },
        { 0x20ab30, agc_glyph, "_AGCGetControllerGlyph" }, { 0x20abf8, agc_get, "_AGCGetController" },
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) mk_override_function(fns[i].off, fns[i].fn, fns[i].name);
    mk_log("plugins: %zu native functions replaced, saves in %s", sizeof(fns) / sizeof(fns[0]), g_save_dir);
}

// ---- touch: the left mouse button acts as one finger ----
// The game is a touch game (iOS); on Windows the engine reports no touches for the mouse. Input's
// touch icalls are wrapped: with real touches (a touchscreen) the engine's answer is passed on,
// otherwise the mouse becomes finger 0, with phases advanced once per frame.
typedef struct {  // UnityEngine.Touch (2019.4)
    int32_t finger_id;
    float pos[2], raw_pos[2], delta[2], time_delta;
    int32_t tap_count, phase, type;
    float pressure, max_pressure, radius, radius_var, altitude, azimuth;
} touch;
_Static_assert(sizeof(touch) == 68, "Touch");
static int32_t (*g_touch_count)(void);
static void (*g_get_touch)(int32_t, touch *);
static void (*g_mouse_pos)(float *);
static uint8_t (*g_mouse_button)(int32_t);
static int32_t (*g_frame_count)(void);
static touch g_mtouch;
static int g_mtouch_active, g_mtouch_frame = -1;

static void mouse_touch_update(void) {
    int f = g_frame_count ? g_frame_count() : 0;
    if (f == g_mtouch_frame) return;
    g_mtouch_frame = f;
    float v[3] = { 0 };
    if (g_mouse_pos) g_mouse_pos(v);
    int down = g_mouse_button && (g_mouse_button(0) & 0xff) && own_window_focused();
    if (down) {
        float dx = v[0] - g_mtouch.pos[0], dy = v[1] - g_mtouch.pos[1];
        if (!g_mtouch_active || g_mtouch.phase == 3) {  // Began
            memset(&g_mtouch, 0, sizeof(g_mtouch));
            g_mtouch.raw_pos[0] = v[0]; g_mtouch.raw_pos[1] = v[1];
            g_mtouch.phase = 0; g_mtouch.tap_count = 1; dx = dy = 0;
        } else {
            g_mtouch.phase = (dx || dy) ? 1 : 2;  // Moved / Stationary
        }
        g_mtouch.pos[0] = v[0]; g_mtouch.pos[1] = v[1];
        g_mtouch.delta[0] = dx; g_mtouch.delta[1] = dy;
        g_mtouch.time_delta = 1 / 60.0f;
        g_mtouch.pressure = g_mtouch.max_pressure = 1;
        g_mtouch_active = 1;
    } else if (g_mtouch_active && g_mtouch.phase != 3) {
        g_mtouch.phase = 3;  // Ended, reported for one frame
        g_mtouch.delta[0] = g_mtouch.delta[1] = 0;
    } else {
        g_mtouch_active = 0;
    }
    static int last_phase = -1;
    int ph = g_mtouch_active ? g_mtouch.phase : -1;
    if (ph != last_phase && ph != 2 && ph != 1) mk_log("mouse touch: phase %d at (%.0f, %.0f) frame %d", ph, g_mtouch.pos[0], g_mtouch.pos[1], f);
    last_phase = ph;
}
static int32_t touch_count(void) {
    int32_t n = g_touch_count ? g_touch_count() : 0;
    if (n) return n;
    mouse_touch_update();
    return g_mtouch_active;
}
static void get_touch(int32_t i, touch *t) {
    if (g_touch_count && g_touch_count() > 0) { g_get_touch(i, t); return; }
    mouse_touch_update();
    *t = g_mtouch;
}
static uint8_t touch_supported(void) { return 1; }

// The iOS game hides and locks the cursor (harmless on a touch screen); on Windows keep it usable.
static void (*g_cursor_visible)(uint8_t);
static void (*g_cursor_lock)(int32_t);
static void cursor_visible(uint8_t v) { (void)v; if (g_cursor_visible) g_cursor_visible(1); }
static void cursor_lock(int32_t m) { (void)m; if (g_cursor_lock) g_cursor_lock(0); }
// Pads reach the game through the AppleGameController replacement. Rewired would also read them
// from Unity's joystick input with the iOS layout (button 0 = d-pad up, so A became up + A):
// report no joysticks to it.
// Rewired (the iOS build) reads pads from Unity's joystick input and maps them with Unity's iOS
// layout, so XInput pads are presented the way the iOS player reports an extended gamepad: the
// name "[extended,wireless] joystick N by ...", A = button 14, B = 13, X = 15, Y = 12, d-pad =
// 4-7, shoulders 8/9, triggers 10/11, pause = 0; sticks on axes 1-4 (Y down positive).
// What Rewired reads is logged (MK_LOG_JOY=0 turns it off).
void *il2cpp_string_new(const char *);  // win_rt.c / api_wrappers.c (U il2cpp_string_new(U))
static void *(*g_joy_names)(void);
static uint8_t (*g_get_key)(int32_t);
static float (*g_axis_raw)(void *), (*g_axis)(void *);
static int log_joy(void) { static int on = -1; if (on < 0) { char v[4] = ""; GetEnvironmentVariableA("MK_LOG_JOY", v, sizeof(v)); on = v[0] != '0'; } return on; }
static int xi_state_of(int pad, xi_state *s) { return g_xinput_get && pad >= 0 && pad < 4 && g_xinput_get((DWORD)pad, s) == 0; }
static int ios_button(int pad, int b) {
    xi_state s;
    if (!xi_state_of(pad, &s)) return 0;
    WORD w = s.pad.buttons;
    switch (b) {
    case 0: return !!(w & 0x10);                 // pause <- Start
    case 4: return !!(w & 0x1);                  // d-pad up
    case 5: return !!(w & 0x8);                  // right
    case 6: return !!(w & 0x2);                  // down
    case 7: return !!(w & 0x4);                  // left
    case 8: return !!(w & 0x100);                // L1
    case 9: return !!(w & 0x200);                // R1
    case 10: return s.pad.lt > 128;              // L2
    case 11: return s.pad.rt > 128;              // R2
    case 12: return !!(w & 0x8000);              // Y
    case 13: return !!(w & 0x2000);              // B
    case 14: return !!(w & 0x1000);              // A
    case 15: return !!(w & 0x4000);              // X
    case 16: return !!(w & 0x20);                // options <- Back (extended "options" button)
    default: return 0;
    }
}
static float ios_axis(int pad, int a) {
    xi_state s;
    if (!xi_state_of(pad, &s)) return 0;
    switch (a) {
    case 1: return stick(s.pad.lx);
    case 2: return -stick(s.pad.ly);
    case 3: return stick(s.pad.rx);
    case 4: return -stick(s.pad.ry);
    default: return 0;
    }
}
static int managed_ascii(void *str, char *out, int n) {
    uint8_t *s8 = str;
    int len = s8 ? *(int32_t *)(s8 + 0x10) : 0, k = 0;
    for (; k < len && k < n - 1; k++) out[k] = (char)((uint16_t *)(s8 + 0x14))[k];
    out[k] = 0;
    return k;
}
static void *joystick_names(void) {
    uint8_t *arr = g_joy_names ? g_joy_names() : NULL;
    if (!arr) return arr;
    uint64_t n = *(uint64_t *)(arr + 0x18);
    for (uint64_t i = 0; i < n && i < 4; i++) {
        xi_state st;
        if (!xi_state_of((int)i, &st)) continue;
        char name[96];
        snprintf(name, sizeof(name), "[extended,wireless] joystick %d by Xbox Wireless Controller", (int)i + 1);
        ((void **)(arr + 0x20))[i] = il2cpp_string_new(name);
    }
    static int logged;
    if (!logged && n) { char nm[128]; managed_ascii(((void **)(arr + 0x20))[0], nm, sizeof(nm)); mk_log("joystick 0 presented as '%s'", nm); logged = 1; }
    return arr;
}
static uint8_t get_key(int32_t code) {
    if (code < 330 || code >= 430) return g_get_key ? g_get_key(code) : 0;
    int b = (code - 330) % 20, r = 0;
    if (code < 350) { for (int p = 0; p < 4 && !r; p++) r = ios_button(p, b); }  // JoystickButtonN: any pad
    else r = ios_button((code - 350) / 20, b);
    if (log_joy()) {
        static uint8_t down[430];
        if (r != down[code]) { down[code] = (uint8_t)r; mk_log("joystick key %d (button %d) %s", code, b, r ? "down" : "up"); }
    }
    return (uint8_t)r;
}
static float joy_axis(void *name, float (*orig)(void *)) {
    char an[64];
    int joy, ax;
    managed_ascii(name, an, sizeof(an));
    if (sscanf(an, "Joy%dAxis%d", &joy, &ax) != 2) return orig ? orig(name) : 0;
    float v = ios_axis(joy - 1, ax);
    if (log_joy()) {
        static int state[5][32];
        int st = v > 0.5f ? 1 : v < -0.5f ? -1 : 0;
        if (joy >= 1 && joy <= 4 && ax >= 0 && ax < 32 && st != state[joy][ax]) { state[joy][ax] = st; mk_log("axis %s = %.2f", an, v); }
    }
    return v;
}
static float axis_raw(void *name) { return joy_axis(name, g_axis_raw); }
static float axis_smooth(void *name) { return joy_axis(name, g_axis); }

// called for each icall the engine registers; returns the implementation to register
void *mk_input_icall(const char *name, void *fn) {
    if (!strcmp(name, "UnityEngine.Cursor::set_visible")) { g_cursor_visible = fn; return (void *)cursor_visible; }
    if (!strcmp(name, "UnityEngine.Cursor::set_lockState")) { g_cursor_lock = fn; return (void *)cursor_lock; }
    if (!strcmp(name, "UnityEngine.Input::GetJoystickNames")) { g_joy_names = fn; return (void *)joystick_names; }
    if (!strcmp(name, "UnityEngine.Input::GetKeyInt")) { g_get_key = fn; return (void *)get_key; }
    if (!strcmp(name, "UnityEngine.Input::GetAxisRaw")) { g_axis_raw = fn; return (void *)axis_raw; }
    if (!strcmp(name, "UnityEngine.Input::GetAxis")) { g_axis = fn; return (void *)axis_smooth; }
    if (!strcmp(name, "UnityEngine.Input::get_touchCount")) { g_touch_count = fn; return (void *)touch_count; }
    if (!strcmp(name, "UnityEngine.Input::GetTouch_Injected")) { g_get_touch = fn; return (void *)get_touch; }
    if (!strcmp(name, "UnityEngine.Input::get_touchSupported")) return (void *)touch_supported;
    if (!strcmp(name, "UnityEngine.Input::get_mousePosition_Injected")) g_mouse_pos = fn;
    if (!strcmp(name, "UnityEngine.Input::GetMouseButton")) g_mouse_button = fn;
    if (!strcmp(name, "UnityEngine.Time::get_frameCount")) g_frame_count = fn;
    if (!strncmp(name, "UnityEngine.Input::GetMouseButton", 33)) mk_log("icall %s", name);
    return fn;
}

// BootstrapTitleScreen.LoadSceneViaButton's `controller.name.Contains(...)` (tools/a64toc.py CALL_SUBST):
// a click on the start button is a touch, so the touch controller is the one recorded.
void mk_title_button_controller(cpu *c) {
    char name[64];
    managed_ascii((void *)(uintptr_t)X(0), name, sizeof(name));
    X(0) = strcmp(name, "Tilt Controller") == 0;
}

// BootstrapVideo.CheckAspectRatio's CloseEnough(target s0, aspect s1, tolerance s2) (tools/a64toc.py
// CALL_SUBST): true when target is the clip ratio nearest to the window's, so any window size gets one.
void mk_nearest_aspect(cpu *c) {
    static const float ratios[] = { 1.33f, 1.43f, 1.6f, 1.77f, 2.16f };
    float target = c->v[0].f[0], aspect = c->v[1].f[0], best = ratios[0];
    for (int i = 1; i < 5; i++)
        if (fabsf(ratios[i] - aspect) < fabsf(best - aspect)) best = ratios[i];
    X(0) = fabsf(target - best) < 0.001f;
}

// ---- debugging: entries of functions translated with A64TOC_TRACE (tools/a64toc.py) ----
// Logs the first calls of each, then every power of two; x0 is `this`, x1 the first argument,
// s0-s2 the first float arguments.
static const struct { uint64_t off; const char *name; } g_trace_names[] = {
    { 0x18f854c, "UIJoystick.OnPointerDown" }, { 0x18f909c, "UIJoystick.OnDrag" }, { 0x18f8594, "UIJoystick.OnPointerUp" },
    { 0x18f7c80, "UIJoystick.Start" }, { 0x18f82d0, "DynamicJoystick.OnPointerDown" }, { 0x18f8cb8, "FloatingJoystick.OnPointerDown" },
    { 0x18f9470, "VariableJoystick.OnPointerDown" }, { 0x18f8eec, "UIJoystick.get_Direction" },
    { 0x1900750, "CustomTiltControl.OnInputUpdate" }, { 0x19001ec, "CustomTiltControl.SetUIJoystick" },
    { 0x190320c, "iOSAttackButton.Initialize" }, { 0x1902ec4, "iOSAttackButton.ToggleJoystick" }, { 0x1902b00, "iOSAttackButton.ToggleTiltControl" },
    { 0x1cb0afc, "iOSMobileHUD.Start" }, { 0x1cb0138, "iOSMobileHUD.ToggleHUD" }, { 0x1cb04c4, "iOSMobileHUD.OnTiltControlToggled" },
    { 0x19a49b0, "UI_ControllerWidget.IsTiltControllerUsed" },
    { 0x1fcf320, "Rewired.TouchJoystick.OnPointerDown" }, { 0x1fd4204, "Rewired.TouchPad.OnPointerDown" },
    { 0x1fd4cb8, "Rewired.TouchRegion.OnPointerDown" }, { 0x1eaf1c8, "Rewired.TouchButton.OnPointerDown" },
    { 0x1eae7d8, "Rewired.TouchInteractable.OnPointerDown" },
    { 0x1e55a44, "RewiredStandaloneInputModule.Process" }, { 0x1e5630c, "RewiredStandaloneInputModule.ProcessTouchEvents" },
    { 0x1e56718, "RewiredStandaloneInputModule.ProcessTouchPress" }, { 0x1e565c4, "RewiredStandaloneInputModule.ProcessMouseEvents" },
    { 0x1e57954, "RewiredStandaloneInputModule.ProcessMousePress" }, { 0x1e55064, "RewiredStandaloneInputModule.ShouldActivateModule" },
    { 0x245b5e8, "StandaloneInputModule.Process" }, { 0x245b7fc, "StandaloneInputModule.ProcessTouchEvents" },
    { 0x245be48, "StandaloneInputModule.ProcessTouchPress" }, { 0x245c8d8, "StandaloneInputModule.ProcessMousePress" },
    { 0x2454c6c, "EventSystem.RaycastAll" }, { 0x246c634, "GraphicRaycaster.Raycast" },
    { 0x1d47704, "Bootstrap.BootNextItem" }, { 0x1d475cc, "Bootstrap.StartBoot" }, { 0x1d47328, "Bootstrap.SkipCurrentBoot" },
    { 0x1d47460, "Bootstrap.CheckFullSkip" }, { 0x1d477e0, "Bootstrap.CheckTapSkip" }, { 0x1d472dc, "Bootstrap.ClickSkip" },
    { 0x1d47734, "Bootstrap.CoBlendToNextBoot" }, { 0x1d63cf8, "BootstrapAppleArcade.Initialize" },
    { 0x1d63e68, "BootstrapAppleArcade.CloseEnough" }, { 0x24559bc, "ExecuteEvents.Execute" }, { 0x246b7f0, "Graphic.Raycast" },
    { 0x246d56c, "GraphicRaycaster.Raycast(canvas)" }, { 0x18f7c2c, "DynamicJoystick.Start" }, { 0x18f8c78, "FloatingJoystick.Start" },
    { 0x18f9430, "VariableJoystick.Start" },
    { 0x19a5a54, "UI_ControllerWidget.ForceTiltController" }, { 0x19a9b18, "UI_ControllerWidget.FindTiltController" },
    { 0x19a5d28, "UI_ControllerWidget.AssignPlayerController" }, { 0x19a854c, "UI_ControllerWidget.SetSinglePlayerController" },
    { 0x19a4154, "UI_ControllerWidget.ClearUsedControllers" }, { 0x19a7374, "UI_ControllerWidget.ReassignUsedControllerNames" },
    { 0x19a7950, "UI_ControllerWidget.AssignOnlyUsedControllersToSystemPlayer" }, { 0x19a9048, "UI_ControllerWidget.Initialize" },
    { 0x19a97a8, "UI_ControllerWidget.FullReinitialize" }, { 0x1cb8e3c, "MenuManager.SetSinglePlayerController" },
    { 0x1d29910, "ControllerSelection.AssignControllerFromTitleScreen" }, { 0x1d232ec, "ControllerSelection.AssignFirstTiltController" },
    { 0x1d29528, "ControllerSelection.AutoAssignControllers" }, { 0x1d275f4, "ControllerSelection.CheckNewPlayerInput" },
    { 0x1d39114, "ControllerSelection_ControllerOption.AssignFirstTiltController" },
    { 0x1d29f30, "ControllerSelection.CheckAutoAssignFromTitleScreen" }, { 0x1d28fd0, "ControllerSelection.InitializeAssignedControllers" },
    { 0x1d65f0c, "BootstrapTitleScreen.LoadSceneViaButton" }, { 0x1d662e4, "BootstrapTitleScreen.CoWaitToLoadScene" },
    { 0x1d66380, "BootstrapTitleScreen.LoadScene" }, { 0x1d66a3c, "BootstrapTitleScreen.SetTitleScreenController" },
    { 0x245e85c, "Button.OnPointerClick" }, { 0x245e7bc, "Button.Press" }, { 0x245e884, "Button.OnSubmit" },
    { 0x2490a70, "Selectable.OnPointerDown" }, { 0x2490ce4, "Selectable.OnPointerUp" }, { 0x1d680dc, "BootstrapVideo.CheckAspectRatio" },
    { 0x1e51338, "RewiredPointerInputModule.GetTouchPointerEventData" }, { 0x23eb29c, "Camera.get_aspect" }, { 0x1d63eec, "BootstrapAppleArcade.CoDelayComplete" },
};
void mk_trace(uint64_t off, cpu *c) {
    // UI raycasts (findings in docs/notes/2026-11-03-pc-recomp-feasibility.md): no longer logged. Naming
    // objects means calling into the engine from inside translated code, which is not safe here.
    if (off == 0x24559bc || off == 0x246b7f0 || off == 0x246d56c || off == 0x246c634 || off == 0x18f8eec) return;
    // string comparisons (A64TOC_TRACE_CALLS): IsTiltControllerUsed's controller name == the tilt
    // controller's name, and the name matches in ForceTiltController / FindTiltController
    if (off == 0x19a4a38 || off == 0x19a5c20 || off == 0x19a9c48 || off == 0x1d299a8 || off == 0x1d29bac) {
        static unsigned n;
        if (n++ >= 300) return;
        char a[96], b[96];
        managed_ascii((void *)(uintptr_t)X(0), a, sizeof(a));
        managed_ascii((void *)(uintptr_t)X(1), b, sizeof(b));
        mk_log("trace %s compares '%s' with '%s' frame %d", off == 0x19a4a38 ? "IsTiltControllerUsed" : off == 0x19a5c20 ? "ForceTiltController" : off == 0x19a9c48 ? "FindTiltController" :
               off == 0x1d299a8 ? "AssignControllerFromTitleScreen (!=)" : "AssignControllerFromTitleScreen (==)",
               a, b, g_frame_count ? g_frame_count() : -1);
        return;
    }
    if ((off == 0x1e56718 || off == 0x245be48) && ((X(2) & 0xff) || (X(3) & 0xff))) {  // ProcessTouchPress(data, pressed, released)
        mk_log("trace %s pressed=%d released=%d frame %d", off == 0x1e56718 ? "RewiredStandaloneInputModule.ProcessTouchPress" : "StandaloneInputModule.ProcessTouchPress",
               (int)(X(2) & 0xff), (int)(X(3) & 0xff), g_frame_count ? g_frame_count() : -1);
        return;
    }
    static uint64_t offs[64];
    static unsigned counts[64];
    int k = 0;
    while (k < 64 && offs[k] && offs[k] != off) k++;
    if (k == 64) return;
    offs[k] = off;
    unsigned n = ++counts[k];
    if (n > 5 && (n & (n - 1))) return;
    const char *name = "?";
    for (size_t i = 0; i < sizeof(g_trace_names) / sizeof(g_trace_names[0]); i++) if (g_trace_names[i].off == off) name = g_trace_names[i].name;
    mk_log("trace %s (%#llx) call %u: x0=%#llx x1=%#llx x2=%#llx x3=%#llx s0-2=%g %g %g frame %d", name, (unsigned long long)off, n,
           (unsigned long long)X(0), (unsigned long long)X(1), (unsigned long long)X(2), (unsigned long long)X(3),
           c->v[0].f[0], c->v[1].f[0], c->v[2].f[0],
           g_frame_count ? g_frame_count() : -1);
}
