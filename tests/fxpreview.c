/* host preview of the colour effects: runs a picture through the scene's effect chain (CPU path, same matrices as
 * the shader) with effects taken from real projects, and simulates the blur shader. Writes raw RGBA strips. */
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "media.h"
#include "mixer.h"
#include "veg.h"
Config g_cfg = { -1, -1, 1, 1, 0, 1, 1, "" };
Session g_s;
void log_printf(const char *fmt, ...) { (void)fmt; }
uint64_t time_us(void) { return 0; }
void mutex_init(Mutex *m, const char *n) { m->id = 1; }
void mutex_lock(Mutex *m) {}
void mutex_unlock(Mutex *m) {}
int thread_start(const char *n, int (*fn)(unsigned, void *), void *a, int p, int s, int c) { return -1; }
void thread_join(int t) {}
#include <psp2/kernel/threadmgr.h>
SceUID sceKernelCreateSema(const char *n, SceUInt a, int i, int m, SceKernelSemaOptParam *o) { return 1; }
int sceKernelWaitSema(SceUID s, int n, SceUInt *t) { return 0; }
int sceKernelSignalSema(SceUID s, int n) { return 0; }
int gpu_shaders_ok(void) { return 0; }
int gpu_pass_ok(int pass) { return 0; }
int gpu_image_alloc(GpuImage *img, int w, int h, int bpp) { return -1; }
int gpu_image_wrap(GpuImage *img, void *d, int w, int h, int bpp) { img->data = d; img->w = w; img->h = h; img->bpp = bpp; img->stride = w; return 0; }
double audio_time(void) { return 0; }
#include <vita2d.h>
static vita2d_texture g_fake_tex[64]; static uint32_t g_fake_px[64][256]; static int g_nfake;
vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat f) { return &g_fake_tex[g_nfake++]; }
void *vita2d_texture_get_datap(const vita2d_texture *t) { return g_fake_px[t - g_fake_tex]; }
void vita2d_texture_set_filters(vita2d_texture *t, SceGxmTextureFilter a, SceGxmTextureFilter b) {}
void vita2d_wait_rendering_done(void) {}
void vita2d_free_texture(vita2d_texture *t) {}
#include "../src/scene.c"

static const VegVfx *find_fx(VegProject *p, int type, const char *name_part)
{
    for (int i = 0; i < p->nevents; i++) for (int k = 0; k < p->events[i].nvfx; k++) {
        VegVfx *v = &p->events[i].vfx[k];
        if (v->type == type && (!name_part || strstr(v->name, name_part))) return v;
    }
    return NULL;
}

/* bilinear sample like the GPU, clamped */
static void sample(const uint8_t *px, int w, int h, float u, float v, float o[4])
{
    float x = u * w - 0.5f, y = v * h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; c++) o[c] = 0;
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        int xx = x0 + i < 0 ? 0 : x0 + i >= w ? w - 1 : x0 + i, yy = y0 + j < 0 ? 0 : y0 + j >= h ? h - 1 : y0 + j;
        float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        for (int c = 0; c < 4; c++) o[c] += wgt * px[(yy * w + xx) * 4 + c];
    }
}

int main(int argc, char **argv)
{
    int w, h, n;
    uint8_t *img = stbi_load(argv[1], &w, &h, &n, 4);
    VegProject lem, nor, scan, shu;
    char err[200];
    veg_load_file(argv[2], &lem, err, 200); veg_load_file(argv[3], &nor, err, 200);
    veg_load_file(argv[4], &scan, err, 200); veg_load_file(argv[5], &shu, err, 200);
    const VegVfx *list[8]; const char *names[8]; int nl = 0;
    list[nl] = find_fx(&lem, VFX_MATRIX, "Red Only"); names[nl++] = "Channel Blend: Red Only";
    list[nl] = find_fx(&lem, VFX_MATRIX, NULL); names[nl++] = "Channel Blend (unnamed = Blue Only)";
    list[nl] = find_fx(&lem, VFX_GRADIENT, "Sunset"); names[nl++] = "Gradient Map: Sunset";
    list[nl] = find_fx(&nor, VFX_LAB, NULL); names[nl++] = "LAB Adjust: Invert Luminosity";
    list[nl] = find_fx(&scan, VFX_LINEARBLUR, NULL); names[nl++] = "Linear Blur (scan.veg)";
    list[nl] = find_fx(&shu, VFX_GAUSSBLUR, NULL); names[nl++] = "Gaussian Blur (Shuric Scan)";
    g_s.proj = lem;   /* gradient textures for Lemming */
    luts_build();
    FILE *out = fopen(argv[6], "wb");
    fprintf(stderr, "%d %d\n", w, h);
    static uint8_t dst[4096 * 4096];
    g_fb_w = w; g_fb_h = h;
    for (int i = 0; i < nl; i++) {
        const VegVfx *v = list[i];
        if (!v) { fprintf(stderr, "missing %s\n", names[i]); continue; }
        fprintf(stderr, "%s\n", names[i]);
        GpuImage src, d;
        gpu_image_wrap(&src, img, w, h, 4);
        gpu_image_wrap(&d, dst, w, h, 4);
        float m[12], m2[12];
        cm_identity(m); cm_identity(m2);
        if (v->type == VFX_LINEARBLUR || v->type == VFX_GAUSSBLUR) {
            GpuLayer L; blur_params(v, &src, &L);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                float u = (x + 0.5f) / w, vv = (y + 0.5f) / h, acc[4] = { 0 }, s[4];
                float du = (L.dir[0] + (u - L.dir[2]) * L.rad[0]) * 0.125f, dv = (L.dir[1] + (vv - L.dir[3]) * L.rad[0]) * 0.125f;
                for (int k = -8; k <= 8; k++) { sample(img, w, h, u + k * du, vv + k * dv, s); for (int c = 0; c < 4; c++) acc[c] += s[c] * L.w[k + 8]; }
                for (int c = 0; c < 4; c++) dst[(y * w + x) * 4 + c] = (uint8_t)(acc[c] + 0.5f);
            }
        } else if (v->type == VFX_GRADIENT) {
            cpu_fx(&src, m, (const uint8_t (*)[3])g_lut[v->lut].rgb, (float)v->amount, m2, &d);
        } else {
            vfx_matrix(v, m);
            cpu_fx(&src, m, NULL, 0, m2, &d);
        }
        fwrite(dst, 1, (size_t)w * h * 4, out);
    }
    fclose(out);
    return 0;
}
