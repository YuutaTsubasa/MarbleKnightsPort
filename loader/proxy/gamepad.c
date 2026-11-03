// Replacement for the game's AppleGameController plugin (Apple GCController, ObjC).
//
// The game polls AppleGameController.GetController(i) every frame. We answer from Unity's
// own joystick input on Android (Input.GetAxisRaw("JoyNAxisM"), Input.GetKey(JoystickNButtonM)),
// using the libunity icall implementations directly. The axis/button mapping can be overridden
// with <external files>/gamepad.txt ("lx=1 ly=-2 ..."; a negative axis is inverted, -99 = unused).
#include <android/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "glyphs.inc"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkpad", __VA_ARGS__)

// AppleGameController.AppleController (blittable, 40 bytes)
typedef struct {
    uint8_t is_valid, is_micro, pad_[2];
    float lx, ly, rx, ry, dx, dy;
    uint8_t any, a, b, x, y, lb, lt, rb, rt, home, menu, options;
} apple_controller;
_Static_assert(sizeof(apple_controller) == 40, "AppleController");

extern const void *mk_find_icall(const char *name);
extern void *il2cpp_string_new(const char *);
extern uint32_t il2cpp_gchandle_new(void *, int);
extern void *il2cpp_domain_get(void);

typedef float (*get_axis_fn)(void *name);
typedef int (*get_key_fn)(int keycode);
typedef void *(*joy_names_fn)(void);
static get_axis_fn g_get_axis;
static get_key_fn g_get_key;
static joy_names_fn g_joy_names;

// mapping: axes are 1-based Unity joystick axis numbers (sign = invert), buttons are Unity
// JoystickButton numbers. Defaults follow Unity's Android mapping for an Xbox-style pad.
static struct { const char *key; int v; } g_map[] = {
    { "lx", 1 }, { "ly", -2 }, { "rx", 3 }, { "ry", -4 }, { "dx", 5 }, { "dy", -6 },
    { "lt_axis", 13 }, { "rt_axis", 12 },
    { "a", 0 }, { "b", 1 }, { "x", 2 }, { "y", 3 }, { "lb", 4 }, { "rb", 5 },
    { "lt", -99 }, { "rt", -99 }, { "menu", 10 }, { "options", 11 }, { "home", -99 }, { 0, 0 } };
static int map(const char *k) { for (int i = 0; g_map[i].key; i++) if (!strcmp(g_map[i].key, k)) return g_map[i].v; return -99; }

#define MAX_PADS 4
#define MAX_AXES 20
static void *g_axis_names[MAX_PADS][MAX_AXES + 1];  // pinned managed strings "JoyNAxisM"
static int g_debug_pad;
static char g_files_dir[512];

void mk_gamepad_init(const char *files_dir, int debug) {
    g_debug_pad = debug;
    snprintf(g_files_dir, sizeof(g_files_dir), "%s", files_dir);
    g_get_axis = (get_axis_fn)mk_find_icall("UnityEngine.Input::GetAxisRaw");
    g_get_key = (get_key_fn)mk_find_icall("UnityEngine.Input::GetKeyInt");
    g_joy_names = (joy_names_fn)mk_find_icall("UnityEngine.Input::GetJoystickNames");
    char path[600];
    snprintf(path, sizeof(path), "%s/gamepad.txt", files_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char k[32];
        int v;
        while (fscanf(f, " %31[^= \n]=%d", k, &v) == 2)
            for (int i = 0; g_map[i].key; i++) if (!strcmp(g_map[i].key, k)) g_map[i].v = v;
        fclose(f);
        LOG("mapping overridden from %s", path);
    }
    LOG("gamepad bridge: GetAxisRaw=%p GetKeyInt=%p GetJoystickNames=%p", g_get_axis, g_get_key, g_joy_names);
}

static float axis(int pad, int a) {
    if (a == -99 || !g_get_axis) return 0;
    int n = a < 0 ? -a : a;
    if (n < 1 || n > MAX_AXES || pad >= MAX_PADS) return 0;
    if (!g_axis_names[pad][n]) {
        char name[32];
        snprintf(name, sizeof(name), "Joy%dAxis%d", pad + 1, n);
        g_axis_names[pad][n] = il2cpp_string_new(name);
        il2cpp_gchandle_new(g_axis_names[pad][n], 1);  // keep it alive
    }
    float v = g_get_axis(g_axis_names[pad][n]);
    return a < 0 ? -v : v;
}
static int button(int pad, int b) {
    if (b < 0 || b > 19 || !g_get_key) return 0;
    return (g_get_key(350 + pad * 20 + b) & 0xff) != 0;  // KeyCode.Joystick1Button0 = 350
}

int mk_gamepad_count(void) {
    if (!g_joy_names) return 0;
    void *arr = g_joy_names();
    if (!arr) return 0;
    size_t len = *(size_t *)((char *)arr + 0x18), n = 0;
    for (size_t i = 0; i < len && i < MAX_PADS; i++) {
        void *s = ((void **)((char *)arr + 0x20))[i];
        if (s && *(int *)((char *)s + 16) > 0) n = i + 1;  // empty names are disconnected slots
    }
    return (int)n;
}

static void debug_scan(int pad) {
    static float last_axes_all[MAX_PADS][MAX_AXES + 1];
    static uint32_t last_buttons_all[MAX_PADS];
    float *last_axes = last_axes_all[pad];
    uint32_t *last_buttons = &last_buttons_all[pad];
    for (int n = 1; n <= MAX_AXES; n++) {
        float v = axis(pad, n);
        if ((v > 0.5f) != (last_axes[n] > 0.5f) || (v < -0.5f) != (last_axes[n] < -0.5f)) LOG("pad %d axis %d = %.2f", pad, n, v);
        last_axes[n] = v;
    }
    uint32_t bits = 0;
    for (int b = 0; b < 20; b++) bits |= (uint32_t)button(pad, b) << b;
    if (bits != *last_buttons) { LOG("pad %d buttons %05x", pad, bits); *last_buttons = bits; }
}

apple_controller mk_gamepad_get(int pad) {
    apple_controller c;
    memset(&c, 0, sizeof(c));
    if (pad < 0 || pad >= mk_gamepad_count()) return c;
    c.is_valid = 1;
    c.lx = axis(pad, map("lx")); c.ly = axis(pad, map("ly"));
    c.rx = axis(pad, map("rx")); c.ry = axis(pad, map("ry"));
    c.dx = axis(pad, map("dx")); c.dy = axis(pad, map("dy"));
    c.a = button(pad, map("a")); c.b = button(pad, map("b"));
    c.x = button(pad, map("x")); c.y = button(pad, map("y"));
    c.lb = button(pad, map("lb")); c.rb = button(pad, map("rb"));
    c.lt = button(pad, map("lt")) || axis(pad, map("lt_axis")) > 0.5f;
    c.rt = button(pad, map("rt")) || axis(pad, map("rt_axis")) > 0.5f;
    c.menu = button(pad, map("menu")); c.options = button(pad, map("options")); c.home = button(pad, map("home"));
    c.any = c.a | c.b | c.x | c.y | c.lb | c.rb | c.lt | c.rt | c.menu | c.options;
    if (g_debug_pad) debug_scan(pad);
    return c;
}

const char *mk_gamepad_glyph(const char *element) {
    static const struct { const char *e, *g; } t[] = {
        { "Button A", "a.circle" }, { "Button B", "b.circle" }, { "Button X", "x.circle" }, { "Button Y", "y.circle" },
        { "Button Menu", "line.horizontal.3.circle" }, { "Button Options", "rectangle.fill.on.rectangle.fill.circle" },
        { "Left Shoulder", "lb.rectangle.roundedbottom" }, { "Left Trigger", "lt.rectangle.roundedtop" },
        { "Right Shoulder", "rb.rectangle.roundedbottom" }, { "Right Trigger", "rt.rectangle.roundedtop" },
        { "Direction Pad", "dpad" }, { "Left Thumbstick", "l.joystick" }, { "Right Thumbstick", "r.joystick" } };
    // The game loads the returned path with File.ReadAllBytes + ImageConversion.LoadImage.
    for (size_t i = 0; element && i < sizeof(t) / sizeof(t[0]); i++) {
        if (strcmp(element, t[i].e)) continue;
        for (size_t k = 0; k < sizeof(g_glyph_pngs) / sizeof(g_glyph_pngs[0]); k++) {
            if (strcmp(g_glyph_pngs[k].name, t[i].g)) continue;
            char dir[600], path[700];
            snprintf(dir, sizeof(dir), "%s/glyphs", g_files_dir);
            snprintf(path, sizeof(path), "%s/%s.png", dir, t[i].g);
            if (access(path, F_OK)) {
                mkdir(dir, 0700);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(g_glyph_pngs[k].png, 1, g_glyph_pngs[k].size, f); fclose(f); }
            }
            return strdup(path);
        }
    }
    return strdup("");
}
