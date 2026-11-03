// FMOD -> waveOut bridge (the Windows version of loader/proxy/fmod_audio.c).
//
// The iOS FMOD in UnityFramework can only output through AudioUnit, so it is started with its
// no-sound output (it still mixes in real time) and a DSP at the head of the master channel group
// copies the final mix into a ring buffer; a waveOut thread plays it. The DSP's read callback is a
// host callback the translated FMOD mixer calls (integer / pointer arguments only).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <string.h>

#include "win_rt.h"

// FMOD_DSP_DESCRIPTION (plugin SDK version 110, FMOD 1.10 - 2.x)
typedef struct {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers, numoutputbuffers;
    uint64_t create, release, reset, read, process, setposition;
    int numparameters;
    uint64_t paramdesc;
    uint64_t setfloat, setint, setbool, setdata, getfloat, getint, getbool, getdata;
    uint64_t shouldiprocess, userdata;
    uint64_t sys_register, sys_deregister, sys_mix;
} fmod_dsp_description;

#define RING_FRAMES (1 << 15)  // ~0.68 s at 48 kHz
static float g_ring[RING_FRAMES * 2];
static volatile LONG g_wr, g_rd;  // frame counters
static int g_rate = 48000;

static uint64_t capture_read(uint64_t state, uint64_t in_, uint64_t out_, uint64_t length_, uint64_t inch_, uint64_t outch_) {
    (void)state;
    const float *in = (const float *)(uintptr_t)in_;
    float *out = (float *)(uintptr_t)out_;
    unsigned length = (unsigned)length_;
    int inch = (int)inch_;
    memcpy(out, in, sizeof(float) * length * inch);
    *(int *)(uintptr_t)outch_ = inch;
    uint32_t w = (uint32_t)g_wr, r = (uint32_t)InterlockedCompareExchange(&g_rd, 0, 0);
    if (w - r + length > RING_FRAMES) return 0;  // the player is behind: drop this block
    for (unsigned i = 0; i < length; i++) {
        float *dst = &g_ring[((w + i) & (RING_FRAMES - 1)) * 2];
        dst[0] = in[i * inch];
        dst[1] = inch > 1 ? in[i * inch + 1] : in[i * inch];
    }
    MemoryBarrier();
    g_wr = (LONG)(w + length);
    return 0;
}

#define NBUF 4
#define BUF_FRAMES 1024
static DWORD WINAPI player(void *arg) {
    (void)arg;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, (DWORD)g_rate, (DWORD)g_rate * 4, 4, 16, 0 };
    HWAVEOUT wo;
    MMRESULT m = waveOutOpen(&wo, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (m != MMSYSERR_NOERROR) { mk_log("audio: waveOutOpen failed (%u)", m); return 0; }
    static int16_t pcm[NBUF][BUF_FRAMES * 2];
    WAVEHDR h[NBUF];
    memset(h, 0, sizeof(h));
    for (int i = 0; i < NBUF; i++) {
        h[i].lpData = (LPSTR)pcm[i];
        h[i].dwBufferLength = sizeof(pcm[i]);
        waveOutPrepareHeader(wo, &h[i], sizeof(h[i]));
        h[i].dwFlags |= WHDR_DONE;
    }
    mk_log("audio: waveOut %d Hz, %d x %d frames", g_rate, NBUF, BUF_FRAMES);
    for (;;) {
        for (int i = 0; i < NBUF; i++) {
            if (!(h[i].dwFlags & WHDR_DONE)) continue;
            uint32_t r = (uint32_t)g_rd, w = (uint32_t)g_wr, avail = w - r;
            if (avail > BUF_FRAMES * 8) { r = w - BUF_FRAMES * 2; avail = w - r; }  // keep latency bounded
            uint32_t n = avail < BUF_FRAMES ? avail : BUF_FRAMES;
            for (uint32_t k = 0; k < n; k++) {
                const float *src = &g_ring[((r + k) & (RING_FRAMES - 1)) * 2];
                for (int ch = 0; ch < 2; ch++) {
                    float v = src[ch] * 32767.0f;
                    pcm[i][k * 2 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            memset(&pcm[i][n * 2], 0, sizeof(int16_t) * 2 * (BUF_FRAMES - n));
            InterlockedExchange(&g_rd, (LONG)(r + n));
            h[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &h[i], sizeof(h[i]));
        }
        WaitForSingleObject(ev, 50);
    }
}

static uint64_t fcall(const char *name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t f = mk_export(name), x[4] = { a, b, c, d };
    if (!f) { mk_log("audio: %s not found", name); return ~0ull; }
    return mk_guest_enter(f, x, 4, NULL, 0, NULL);
}

// right after FMOD Studio initialized on the no-sound output
void mk_fmod_attach_output(uint64_t core) {
    unsigned ver = 0;
    int rate = 48000, mode = 0, raw = 0;
    fcall("_FMOD5_System_GetVersion", core, (uint64_t)(uintptr_t)&ver, 0, 0);
    fcall("_FMOD5_System_GetSoftwareFormat", core, (uint64_t)(uintptr_t)&rate, (uint64_t)(uintptr_t)&mode, (uint64_t)(uintptr_t)&raw);
    static fmod_dsp_description desc;
    desc.pluginsdkversion = 110;
    strcpy(desc.name, "waveOut capture");
    desc.version = 1;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = mk_host_callback((void *)capture_read, "FMOD capture DSP read");
    uint64_t dsp = 0, cg = 0;
    int r1 = (int)fcall("_FMOD5_System_CreateDSP", core, (uint64_t)(uintptr_t)&desc, (uint64_t)(uintptr_t)&dsp, 0);
    int r2 = (int)fcall("_FMOD5_System_GetMasterChannelGroup", core, (uint64_t)(uintptr_t)&cg, 0, 0);
    int r3 = dsp && cg ? (int)fcall("_FMOD5_ChannelGroup_AddDSP", cg, (uint64_t)-1 /* DSP_HEAD */, dsp, 0) : -1;
    int r4 = dsp ? (int)fcall("_FMOD5_DSP_SetActive", dsp, 1, 0, 0) : -1;
    mk_log("FMOD %x.%02x.%02x, software format %d Hz mode %d; capture DSP create=%d master=%d add=%d active=%d",
           ver >> 16, (ver >> 8) & 0xff, ver & 0xff, rate, mode, r1, r2, r3, r4);
    if (r1 || r3) return;
    g_rate = rate > 0 ? rate : 48000;
    CreateThread(NULL, 0, player, NULL, 0, NULL);
}
