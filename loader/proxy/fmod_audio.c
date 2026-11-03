// FMOD -> AAudio bridge.
//
// The iOS FMOD in UnityFramework can only output through AudioUnit/AVAudioSession, which we do
// not provide. FMOD is therefore started with its no-sound output (it still mixes in real time),
// and a small DSP at the head of the master channel group copies the final mix into a ring
// buffer that an AAudio stream plays. The two clocks drift slightly; the ring buffer absorbs it
// by dropping or padding a little audio.
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "darwin.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mkaudio", __VA_ARGS__)

// FMOD_DSP_DESCRIPTION (plugin SDK version 110, FMOD 1.10 - 2.x)
typedef struct {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers, numoutputbuffers;
    void *create, *release, *reset;
    int (*read)(void *state, float *in, float *out, unsigned int length, int inchannels, int *outchannels);
    void *process, *setposition;
    int numparameters;
    void *paramdesc;
    void *setfloat, *setint, *setbool, *setdata, *getfloat, *getint, *getbool, *getdata;
    void *shouldiprocess;
    void *userdata;
    void *sys_register, *sys_deregister, *sys_mix;
} fmod_dsp_description;

#define RING_FRAMES (1 << 15)  // ~0.68 s at 48 kHz
static float g_ring[RING_FRAMES * 2];
static _Atomic uint32_t g_wr, g_rd;    // frame counters
static int g_channels = 2;
static AAudioStream *g_stream;

static int capture_read(void *state, float *in, float *out, unsigned int length, int inch, int *outch) {
    (void)state;
    memcpy(out, in, sizeof(float) * length * inch);
    *outch = inch;
    uint32_t w = atomic_load_explicit(&g_wr, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&g_rd, memory_order_acquire);
    if (w - r + length > RING_FRAMES) return 0;  // consumer is behind: drop this block
    for (unsigned int i = 0; i < length; i++) {
        float *dst = &g_ring[((w + i) & (RING_FRAMES - 1)) * 2];
        dst[0] = in[i * inch];
        dst[1] = inch > 1 ? in[i * inch + 1] : in[i * inch];
    }
    atomic_store_explicit(&g_wr, w + length, memory_order_release);
    return 0;
}

static aaudio_data_callback_result_t play(AAudioStream *s, void *user, void *audio, int32_t frames) {
    (void)s; (void)user;
    float *out = audio;
    uint32_t r = atomic_load_explicit(&g_rd, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&g_wr, memory_order_acquire);
    uint32_t avail = w - r;
    // keep latency bounded: if far ahead, skip forward
    if (avail > (uint32_t)frames * 8) { r = w - frames * 4; avail = w - r; }
    int32_t n = avail < (uint32_t)frames ? (int32_t)avail : frames;
    for (int32_t i = 0; i < n; i++) {
        const float *src = &g_ring[((r + i) & (RING_FRAMES - 1)) * 2];
        out[i * 2] = src[0];
        out[i * 2 + 1] = src[1];
    }
    memset(out + n * 2, 0, sizeof(float) * 2 * (frames - n));
    atomic_store_explicit(&g_rd, r + n, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void start_aaudio(int rate) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { LOG("AAudio builder failed"); return; }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, play, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { LOG("AAudio open failed: %s", AAudio_convertResultToText(r)); g_stream = NULL; return; }
    r = AAudioStream_requestStart(g_stream);
    LOG("AAudio stream: %d Hz, %d frames/burst, start=%s", AAudioStream_getSampleRate(g_stream),
        AAudioStream_getFramesPerBurst(g_stream), AAudio_convertResultToText(r));
}

// Called right after FMOD Studio initialized on the no-sound output.
void mk_fmod_attach_output(mk_image *img, void *core) {
    int (*create_dsp)(void *, const fmod_dsp_description *, void **) = mk_export(img, "_FMOD5_System_CreateDSP");
    int (*master)(void *, void **) = mk_export(img, "_FMOD5_System_GetMasterChannelGroup");
    int (*add_dsp)(void *, int, void *) = mk_export(img, "_FMOD5_ChannelGroup_AddDSP");
    int (*set_active)(void *, int) = mk_export(img, "_FMOD5_DSP_SetActive");
    int (*get_format)(void *, int *, int *, int *) = mk_export(img, "_FMOD5_System_GetSoftwareFormat");
    int (*get_version)(void *, unsigned *) = mk_export(img, "_FMOD5_System_GetVersion");
    unsigned ver = 0;
    int rate = 48000, mode = 0, raw = 0;
    get_version(core, &ver);
    get_format(core, &rate, &mode, &raw);
    static fmod_dsp_description desc;
    desc.pluginsdkversion = 110;
    strcpy(desc.name, "AAudio capture");
    desc.version = 1;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = capture_read;
    void *dsp = NULL, *cg = NULL;
    int r1 = create_dsp(core, &desc, &dsp);
    int r2 = master(core, &cg);
    int r3 = dsp && cg ? add_dsp(cg, -1 /* FMOD_CHANNELCONTROL_DSP_HEAD */, dsp) : -1;
    int r4 = dsp ? set_active(dsp, 1) : -1;
    LOG("FMOD %x.%02x.%02x, software format %d Hz mode %d; capture DSP create=%d master=%d add=%d active=%d",
        ver >> 16, (ver >> 8) & 0xff, ver & 0xff, rate, mode, r1, r2, r3, r4);
    (void)g_channels;
    if (!r1 && !r3) start_aaudio(rate);
}

// App to background / foreground (from the engine's pause notifications, see proxy.c).
void mk_audio_pause(int paused) {
    if (!g_stream) return;
    if (paused) AAudioStream_requestPause(g_stream);
    else AAudioStream_requestStart(g_stream);
}
