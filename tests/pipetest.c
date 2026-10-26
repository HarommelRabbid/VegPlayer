/* host integration test: real project + fake media through the scene builder and the mixer */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "media.h"
#include "mixer.h"
#include "veg.h"
Config g_cfg = { -1, -1, 1, 1, 0, 1, "" };
Session g_s;
void log_printf(const char *fmt, ...) { (void)fmt; }
uint64_t time_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
void mutex_init(Mutex *m, const char *n) { (void)n; m->id = 1; }
void mutex_lock(Mutex *m) { (void)m; }
void mutex_unlock(Mutex *m) { (void)m; }
int thread_start(const char *n, int (*fn)(unsigned, void *), void *a, int p, int s, int c) { return -1; }
void thread_join(int t) {}
#include <psp2/kernel/threadmgr.h>
SceUID sceKernelCreateSema(const char *n, SceUInt a, int i, int m, SceKernelSemaOptParam *o) { return 1; }
int sceKernelWaitSema(SceUID s, int n, SceUInt *t) { return 0; }
int sceKernelSignalSema(SceUID s, int n) { return 0; }
int gpu_shaders_ok(void) { return 1; }
int gpu_image_alloc(GpuImage *img, int w, int h, int bpp) { return -1; }
int gpu_image_wrap(GpuImage *img, void *d, int w, int h, int bpp) { img->data = d; img->w = w; img->h = h; img->bpp = bpp; img->stride = (w + 7) & ~7; return 0; }
double audio_time(void) { return 0; }
#include "../src/scene.c"

int main(int argc, char **argv)
{
    char err[256];
    if (veg_load_file(argv[1], &g_s.proj, err, sizeof err)) { printf("load: %s\n", err); return 1; }
    VegProject *p = &g_s.proj;
    g_s.m = calloc((size_t)p->nmedia, sizeof(MediaRt));
    static volatile int on[512];
    for (int t = 0; t < p->ntracks; t++) on[t] = 1;
    g_s.track_on = on;
    static uint8_t px[64 * 36 * 4];
    for (int i = 0; i < p->nmedia; i++) {
        VideoCache *vc = &g_s.m[i].vc;
        vc->ok = 1; vc->src_w = 1280; vc->src_h = 720; vc->nframes = 30 * 20; vc->duration = 20;
        vc->pts = malloc(sizeof(float) * vc->nframes); vc->slot = malloc(sizeof(int) * vc->nframes);
        vc->nimg = 4; vc->img = calloc(4, sizeof(GpuImage));
        for (int k = 0; k < 4; k++) gpu_image_wrap(&vc->img[k], px, 64, 36, 4);
        for (int k = 0; k < vc->nframes; k++) { vc->pts[k] = k / 30.0f; vc->slot[k] = k % 4; }
        AudioCache *ac = &g_s.m[i].ac;
        ac->ok = 1; ac->rate = 44100; ac->total = 44100 * 200; ac->nseg = 1;
        ac->seg = calloc(1, sizeof(PcmSeg)); ac->seg[0].start = 0; ac->seg[0].count = ac->total;
        ac->seg[0].pcm = malloc(sizeof(int16_t) * 2 * ac->total);
        for (int64_t k = 0; k < ac->total; k++) ac->seg[0].pcm[2 * k] = ac->seg[0].pcm[2 * k + 1] = (int16_t)(8000 * sin(k * 0.05) + 3000 * sin(k * 0.011));
    }
    g_vx = 0; g_vy = 2; g_vw = 960; g_vh = 540;
    /* video: build a scene every 1/30 s */
    static Scene sc;
    int maxl = 0, bad = 0, frames = 0;
    uint64_t t0 = time_us();
    for (double T = 0; T < p->duration; T += 1.0 / 30, frames++) {
        build(&sc, 0, T);
        if (sc.n > maxl) maxl = sc.n;
        for (int i = 0; i < sc.n; i++) for (int k = 0; k < 4; k++)
            if (!isfinite(sc.layer[i].x[k]) || !isfinite(sc.layer[i].y[k]) || fabsf(sc.layer[i].x[k]) > 1e6f) bad++;
    }
    uint64_t t1 = time_us();
    printf("scenes: %d built, max layers %d, bad coords %d, %.1f us/scene on host\n", frames, maxl, bad, (double)(t1 - t0) / frames);
    /* a peek at one busy moment */
    build(&sc, 0, p->duration * 0.5);
    for (int i = 0; i < sc.n && i < 4; i++) printf("  layer %d: (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f) a=%.2f fx=%d m0=%.2f\n", i,
        sc.layer[i].x[0], sc.layer[i].y[0], sc.layer[i].x[1], sc.layer[i].y[1], sc.layer[i].x[2], sc.layer[i].y[2], sc.layer[i].x[3], sc.layer[i].y[3],
        sc.layer[i].tint[3], sc.layer[i].fx, sc.layer[i].m[0]);
    /* audio: mix the whole project */
    mixer_init();
    int total = (int)(p->duration * OUT_RATE);
    float *blk = malloc(sizeof(float) * 2048);
    int maxv = 0; double peak = 0;
    t0 = time_us();
    for (int s = 0; s < total; s += 1024) {
        mixer_mix(blk, 1024, s);
        int v = mixer_active_voices(); if (v > maxv) maxv = v;
        for (int k = 0; k < 2048; k++) if (fabs(blk[k]) > peak) peak = fabs(blk[k]);
    }
    t1 = time_us();
    printf("mixer: %.1f s of audio in %.2f s on host (%.0fx realtime), max voices %d, peak %.2f\n", p->duration, (t1 - t0) / 1e6,
           p->duration / ((t1 - t0) / 1e6), maxv, peak);
    return 0;
}
