/* scale.c - box-filter downscaler from RGBA8888 (decoder output / decoded pictures) to a cache image.
 * Portable: only touches memory. Source rows are first copied into a cached scratch buffer because the
 * hardware decoder writes to uncached memory, where strided per-pixel reads are slow. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "media.h"

#define MAXK 4

size_t downscale_scratch_size(int sw, int sh, int dw, int dh)
{
    (void)sh; (void)dh;
    return (size_t)MAXK * (size_t)sw * 4 + (size_t)dw * MAXK * sizeof(int) + 64;
}

static inline int kfor(double span) { int k = (int)(span + 0.5); return k < 1 ? 1 : k > MAXK ? MAXK : k; }

void downscale_rgba(const uint8_t *src, int pitch, int sw, int sh, GpuImage *dst, int keep_alpha, uint8_t *scratch)
{
    int dw = dst->w, dh = dst->h;
    double rx = (double)sw / dw, ry = (double)sh / dh;
    int kx = kfor(rx), ky = kfor(ry);
    uint8_t *rows = scratch;
    int *xs = (int *)(((uintptr_t)(scratch + (size_t)MAXK * sw * 4) + 15) & ~(uintptr_t)15);
    for (int x = 0; x < dw; x++)
        for (int j = 0; j < kx; j++) {
            int sx = (int)((x + (j + 0.5) / kx) * rx);
            xs[x * kx + j] = (sx < 0 ? 0 : sx >= sw ? sw - 1 : sx) * 4;
        }
    const uint32_t inv = 65536u / (uint32_t)(kx * ky);
    for (int y = 0; y < dh; y++) {
        for (int j = 0; j < ky; j++) {
            int sy = (int)((y + (j + 0.5) / ky) * ry);
            sy = sy < 0 ? 0 : sy >= sh ? sh - 1 : sy;
            memcpy(rows + (size_t)j * sw * 4, src + (size_t)sy * (size_t)pitch, (size_t)sw * 4);
        }
        uint8_t *out = (uint8_t *)dst->data + (size_t)y * (size_t)dst->stride * (size_t)dst->bpp;
        for (int x = 0; x < dw; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            const int *xx = xs + x * kx;
            for (int j = 0; j < ky; j++) {
                const uint8_t *row = rows + (size_t)j * sw * 4;
                for (int i = 0; i < kx; i++) {
                    const uint8_t *p = row + xx[i];
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                }
            }
            r = (r * inv) >> 16; g = (g * inv) >> 16; b = (b * inv) >> 16; a = (a * inv) >> 16;
            if (dst->bpp == 2) {
                ((uint16_t *)out)[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            } else {
                ((uint32_t *)out)[x] = r | g << 8 | b << 16 | (keep_alpha ? a : 255u) << 24;
            }
        }
    }
}

/* ------------------------------------------------------------------ colour matrices */

void cm_identity(float m[12])
{
    static const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    memcpy(m, I, sizeof I);
}

void cm_mul(float o[12], const float a[12], const float b[12])
{
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j];
        r[i * 4 + 3] = a[i * 4 + 0] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    memcpy(o, r, sizeof r);
}

int cm_is_identity(const float m[12])
{
    float I[12];
    cm_identity(I);
    for (int i = 0; i < 12; i++) if (fabsf(m[i] - I[i]) > 1e-4f) return 0;
    return 1;
}

