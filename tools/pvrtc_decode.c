// PVRTC1 4bpp -> RGBA8 decoder, for converting the iOS textures of the Windows build
// (tools/win_textures.py loads it with ctypes; build: see that script).
//
// Each 4x4 block is 64 bits in Morton order: the modulation word (2 bits per pixel), then the
// colour word (bit 0 = punch-through mode, bits 1-15 colour A, bits 16-31 colour B). Colours A and
// B form two low-resolution images that are bilinearly upscaled with each block's value at its
// centre (wrapping at the edges); the modulation blends between them per pixel.
#include <stdint.h>
#include <string.h>

typedef struct { int r, g, b, a; } px;  // 8-bit channels

static uint32_t twiddle(uint32_t bw, uint32_t bh, uint32_t x, uint32_t y) {
    uint32_t mn = bw < bh ? bw : bh, maxv = bh < bw ? x : y, t = 0, src = 1, dst = 1, shift = 0;
    while (src < mn) {
        if (y & src) t |= dst;
        if (x & src) t |= dst << 1;
        src <<= 1; dst <<= 2; shift++;
    }
    return t | ((maxv >> shift) << (2 * shift));
}
static int x5(int v) { return (v << 3) | (v >> 2); }
static int x4(int v) { return (v << 4) | v; }
static int x3(int v) { return (v << 5) | (v << 2) | (v >> 1); }
static px colour_a(uint32_t c) {
    px p;
    if (c & 0x8000) { p.r = x5((c >> 10) & 31); p.g = x5((c >> 5) & 31); p.b = x5((((c >> 1) & 15) << 1) | (((c >> 1) & 15) >> 3)); p.a = 255; }
    else { p.a = x3((c >> 12) & 7); p.r = x4((c >> 8) & 15); p.g = x4((c >> 4) & 15); p.b = x3((c >> 1) & 7); }
    return p;
}
static px colour_b(uint32_t c) {
    px p;
    if (c & 0x80000000u) { p.r = x5((c >> 26) & 31); p.g = x5((c >> 21) & 31); p.b = x5((c >> 16) & 31); p.a = 255; }
    else { p.a = x3((c >> 28) & 7); p.r = x4((c >> 24) & 15); p.g = x4((c >> 20) & 15); p.b = x4((c >> 16) & 15); }
    return p;
}

// src: max(w,8) x max(h,8) PVRTC 4bpp data; out: w x h RGBA8 (the top-left part of the decoded image)
__declspec(dllexport) int pvrtc4_decode(const uint8_t *src, int w, int h, uint8_t *out) {
    int pw = w < 8 ? 8 : w, ph = h < 8 ? 8 : h;
    if ((pw & (pw - 1)) || (ph & (ph - 1))) return -1;  // PVRTC1 needs powers of two
    uint32_t bw = (uint32_t)pw / 4, bh = (uint32_t)ph / 4;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // the four blocks whose centres surround this pixel, and the weights
            int fx = x - 2, fy = y - 2;
            int bx0 = (fx >= 0 ? fx : fx - 3) / 4, by0 = (fy >= 0 ? fy : fy - 3) / 4;
            int wx = fx - bx0 * 4, wy = fy - by0 * 4;  // 0..3
            px A[4], B[4];
            for (int k = 0; k < 4; k++) {
                uint32_t bx = (uint32_t)((bx0 + (k & 1) + (int)bw) % (int)bw), by = (uint32_t)((by0 + (k >> 1) + (int)bh) % (int)bh);
                const uint8_t *blk = src + 8 * (size_t)twiddle(bw, bh, bx, by);
                uint32_t col;
                memcpy(&col, blk + 4, 4);
                A[k] = colour_a(col);
                B[k] = colour_b(col);
            }
            int w00 = (4 - wx) * (4 - wy), w10 = wx * (4 - wy), w01 = (4 - wx) * wy, w11 = wx * wy;  // sum 16
            px a = { (A[0].r * w00 + A[1].r * w10 + A[2].r * w01 + A[3].r * w11 + 8) / 16,
                     (A[0].g * w00 + A[1].g * w10 + A[2].g * w01 + A[3].g * w11 + 8) / 16,
                     (A[0].b * w00 + A[1].b * w10 + A[2].b * w01 + A[3].b * w11 + 8) / 16,
                     (A[0].a * w00 + A[1].a * w10 + A[2].a * w01 + A[3].a * w11 + 8) / 16 };
            px b = { (B[0].r * w00 + B[1].r * w10 + B[2].r * w01 + B[3].r * w11 + 8) / 16,
                     (B[0].g * w00 + B[1].g * w10 + B[2].g * w01 + B[3].g * w11 + 8) / 16,
                     (B[0].b * w00 + B[1].b * w10 + B[2].b * w01 + B[3].b * w11 + 8) / 16,
                     (B[0].a * w00 + B[1].a * w10 + B[2].a * w01 + B[3].a * w11 + 8) / 16 };
            // this pixel's own block: modulation bits and mode
            const uint8_t *own = src + 8 * (size_t)twiddle(bw, bh, (uint32_t)x / 4, (uint32_t)y / 4);
            uint32_t mod, col;
            memcpy(&mod, own, 4);
            memcpy(&col, own + 4, 4);
            int m = (mod >> (2 * ((y & 3) * 4 + (x & 3)))) & 3, wgt, punch = 0;
            if (col & 1) { static const int pt[4] = { 0, 4, 4, 8 }; wgt = pt[m]; punch = m == 2; }
            else { static const int st[4] = { 0, 3, 5, 8 }; wgt = st[m]; }
            uint8_t *o = out + 4 * ((size_t)y * w + x);
            o[0] = (uint8_t)((a.r * (8 - wgt) + b.r * wgt) / 8);
            o[1] = (uint8_t)((a.g * (8 - wgt) + b.g * wgt) / 8);
            o[2] = (uint8_t)((a.b * (8 - wgt) + b.b * wgt) / 8);
            o[3] = punch ? 0 : (uint8_t)((a.a * (8 - wgt) + b.a * wgt) / 8);
        }
    }
    return 0;
}
