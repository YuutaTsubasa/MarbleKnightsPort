// Tilt controls. CustomTiltControl reads Unity's Input.gyro, which the Android player implements
// with the rotation-vector sensor, so only the game's MagneticHeading plugin (iOS CLLocationManager
// compass, used to keep the heading steady) needs replacing. We report "no heading": the game then
// uses the gyro attitude alone.
#include <android/log.h>
#include <stdint.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mktilt", __VA_ARGS__)

extern const void *mk_find_icall(const char *name);

static void mh_start(void) { LOG("MagneticHeading start (no compass: heading unavailable)"); }
static void mh_stop(void) {}
static int mh_available(void) { return 0; }
static float mh_accuracy(void) { return -1; }
static float mh_heading(void) { return 0; }
typedef struct { float x, y, z; } vec3;
static vec3 mh_vector(void) { vec3 v = { 0, 0, 0 }; return v; }

typedef struct { uintptr_t off; void *fn; const char *name; } mk_native;
const mk_native mk_tilt_natives[] = {
    { 0x2092e4, mh_start, "_MHStart" }, { 0x209338, mh_stop, "_MHStop" },
    { 0x209380, mh_available, "_MHHeadingAvailable" }, { 0x2093d4, mh_accuracy, "_MHHeadingAccuracy" },
    { 0x209430, mh_heading, "_MHGetMagneticHeading" }, { 0x20948c, mh_vector, "_MHGetMagneticVector" },
    { 0, 0, 0 } };

// debug: log the gyro state (called with the fps line). The game only turns the gyro on inside a
// level with tilt selected, so in debug mode we switch it on ourselves to check the sensor path.
void mk_tilt_debug(void) {
    static int (*enabled)(int);
    static void (*set_enabled)(int, int);
    static void (*attitude)(int, float *);
    if (!enabled) {
        enabled = (int (*)(int))mk_find_icall("UnityEngine.Gyroscope::getEnabled_Internal");
        set_enabled = (void (*)(int, int))mk_find_icall("UnityEngine.Gyroscope::setEnabled_Internal");
        attitude = (void (*)(int, float *))mk_find_icall("UnityEngine.Gyroscope::attitude_Internal_Injected");
        if (!enabled || !attitude || !set_enabled) {
            LOG("gyro icalls not found (%p %p %p)", enabled, set_enabled, attitude);
            enabled = (void *)1;
            return;
        }
        set_enabled(0, 1);
    }
    if (enabled == (void *)1) return;
    float q[4] = { 0 };
    attitude(0, q);
    LOG("gyro enabled=%d attitude=(%.3f %.3f %.3f %.3f)", enabled(0) & 0xff, q[0], q[1], q[2], q[3]);
}
