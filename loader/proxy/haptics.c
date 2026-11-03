// NiceVibrations (iOS UIFeedbackGenerator haptics) -> Android Vibrator predefined effects.
//
// The game calls the iOS natives SelectionHaptic(), LightImpactHaptic(), ... from the main
// thread, which Unity has attached to the JVM. We look up the Vibrator through
// ActivityThread.currentApplication() (a framework class, so the default class loader finds it)
// and play VibrationEffect.createPredefined(...) (API 29+).
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdint.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkhaptic", __VA_ARGS__)

enum { EFFECT_CLICK = 0, EFFECT_DOUBLE_CLICK = 1, EFFECT_TICK = 2, EFFECT_HEAVY_CLICK = 5 };

static JavaVM *g_vm;
static jobject g_vibrator;
static jclass g_effect_class;
static jmethodID g_create_predefined, g_vibrate;
static int g_failed;

static JNIEnv *env(void) {
    if (!g_vm) {
        jint (*get_vms)(JavaVM **, jsize, jsize *) = dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        if (!get_vms) {
            void *h = dlopen("libnativehelper.so", RTLD_NOW);
            if (h) get_vms = dlsym(h, "JNI_GetCreatedJavaVMs");
        }
        jsize n = 0;
        if (!get_vms || get_vms(&g_vm, 1, &n) != JNI_OK || n < 1) { g_vm = NULL; return NULL; }
    }
    JNIEnv *e = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK &&
        (*g_vm)->AttachCurrentThread(g_vm, &e, NULL) != JNI_OK)
        return NULL;
    return e;
}

static int clear(JNIEnv *e) {
    if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return 1; }
    return 0;
}

static int setup(JNIEnv *e) {
    if (g_vibrator) return 1;
    if (g_failed) return 0;
    jclass at = (*e)->FindClass(e, "android/app/ActivityThread");
    jmethodID cur = at ? (*e)->GetStaticMethodID(e, at, "currentApplication", "()Landroid/app/Application;") : NULL;
    jobject app = cur ? (*e)->CallStaticObjectMethod(e, at, cur) : NULL;
    jclass ctx = (*e)->FindClass(e, "android/content/Context");
    jmethodID gss = ctx ? (*e)->GetMethodID(e, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
    jstring name = (*e)->NewStringUTF(e, "vibrator");
    jobject vib = (app && gss) ? (*e)->CallObjectMethod(e, app, gss, name) : NULL;
    jclass vc = (*e)->FindClass(e, "android/os/Vibrator");
    jclass ec = (*e)->FindClass(e, "android/os/VibrationEffect");
    if (clear(e) || !vib || !vc || !ec) { g_failed = 1; LOG("no vibrator available"); return 0; }
    g_vibrate = (*e)->GetMethodID(e, vc, "vibrate", "(Landroid/os/VibrationEffect;)V");
    g_create_predefined = (*e)->GetStaticMethodID(e, ec, "createPredefined", "(I)Landroid/os/VibrationEffect;");
    if (clear(e) || !g_vibrate || !g_create_predefined) { g_failed = 1; LOG("VibrationEffect API missing"); return 0; }
    g_vibrator = (*e)->NewGlobalRef(e, vib);
    g_effect_class = (*e)->NewGlobalRef(e, ec);
    LOG("vibrator ready");
    return 1;
}

static void play(int effect) {
    JNIEnv *e = env();
    if (!e || !setup(e)) return;
    (*e)->PushLocalFrame(e, 4);
    jobject fx = (*e)->CallStaticObjectMethod(e, g_effect_class, g_create_predefined, effect);
    if (fx) (*e)->CallVoidMethod(e, g_vibrator, g_vibrate, fx);
    clear(e);
    (*e)->PopLocalFrame(e, NULL);
}

static void hap_noop(void) {}
static void hap_selection(void) { play(EFFECT_TICK); }
static void hap_light(void) { play(EFFECT_TICK); }
static void hap_medium(void) { play(EFFECT_CLICK); }
static void hap_heavy(void) { play(EFFECT_HEAVY_CLICK); }
static void hap_success(void) { play(EFFECT_DOUBLE_CLICK); }
static void hap_warning(void) { play(EFFECT_DOUBLE_CLICK); }
static void hap_failure(void) { play(EFFECT_HEAVY_CLICK); }

// offsets of the iOS natives in UnityFramework 2.3.1 (targets of the MMVibrationManager externs)
typedef struct { uintptr_t off; void *fn; const char *name; } mk_native;
const mk_native mk_haptic_natives[] = {
    { 0x20901c, hap_noop, "InstantiateFeedbackGenerators" }, { 0x2090f4, hap_noop, "ReleaseFeedbackGenerators" },
    { 0x209150, hap_selection, "SelectionHaptic" }, { 0x209188, hap_success, "SuccessHaptic" },
    { 0x2091c4, hap_warning, "WarningHaptic" }, { 0x209200, hap_failure, "FailureHaptic" },
    { 0x20923c, hap_light, "LightImpactHaptic" }, { 0x209274, hap_medium, "MediumImpactHaptic" },
    { 0x2092ac, hap_heavy, "HeavyImpactHaptic" }, { 0, 0, 0 } };

// debug: one click to check the JNI path (main thread)
void mk_haptic_test(void) { play(EFFECT_CLICK); }
