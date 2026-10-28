/* host test of the track reverb: a 0.5 s noise burst on a track with scan.veg's Sony Reverb settings;
 * prints the wet/dry balance during the burst, the decay time (RT60) and the cost */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "media.h"
#include "mixer.h"
Session g_s;
Config g_cfg = { -1, -1, 1, 1, 0, 1, 1, 0, 1, 1, 1, "" };
void log_printf(const char *fmt, ...) { (void)fmt; }

static double rms(const float *x, int a, int b) { double s = 0; for (int i = a; i < b; i++) s += (double)x[2 * i] * x[2 * i]; return sqrt(s / (b - a)); }

int main(void)
{
    VegProject *p = &g_s.proj;
    p->ntracks = 1; p->tracks = calloc(1, sizeof(VegTrack));
    p->nevents = 1; p->events = calloc(1, sizeof(VegEvent));
    p->nmedia = 1; p->media = calloc(1, sizeof(VegMedia));
    g_s.m = calloc(1, sizeof(MediaRt));
    static int on[1] = { 1 };
    g_s.track_on = on;
    AudioCache *ac = &g_s.m[0].ac;
    ac->ok = 1; ac->rate = 48000; ac->total = 48000; ac->nseg = 1;
    ac->seg = calloc(1, sizeof(PcmSeg));
    ac->seg[0].start = 0; ac->seg[0].count = ac->total;
    ac->seg[0].pcm = malloc(sizeof(int16_t) * 2 * ac->total);
    srand(3);
    for (int i = 0; i < ac->total; i++) ac->seg[0].pcm[2 * i] = ac->seg[0].pcm[2 * i + 1] = (int16_t)((rand() % 16000) - 8000);
    VegTrack *t = &p->tracks[0];
    t->kind = VEG_KIND_AUDIO; t->level = 1; t->first_event = 0; t->nevents = 1;
    static VegAfx rv;
    rv.type = AFX_REVERB; rv.dry_db = 0; rv.wet_db = -3.5; rv.early_db = -2; rv.decay_s = 2.5; rv.predelay_ms = 30; rv.hicut_hz = 12000;
    t->afx = &rv; t->nafx = 1;
    VegEvent *e = &p->events[0];
    e->track = 0; e->kind = VEG_KIND_AUDIO; e->media = 0; e->has_take = 1; e->level = 1; e->start = 0; e->length = 0.5; e->rate = 1;
    p->duration = 5;
    int total = 48000 * 5;
    float *out = calloc((size_t)total * 2, sizeof(float));
    mixer_init();
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int s0 = 0; s0 < total; s0 += 1024) mixer_mix(out + 2 * s0, total - s0 < 1024 ? total - s0 : 1024, s0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
    /* dry level = rms of the source; wet tail just after the burst */
    double src = 8000 / 32768.0 / sqrt(3.0);
    double during = rms(out, 4800, 24000), tail0 = rms(out, 24000 + 2400, 24000 + 7200);
    printf("source rms %.3f, during burst %.3f, tail just after %.3f (%.1f dB under the source)\n", src, during, tail0, 20 * log10(tail0 / src));
    /* RT60 from the decay slope between 0.3 s and 1.3 s after the burst */
    double e1 = rms(out, 24000 + 14400, 24000 + 19200), e2 = rms(out, 24000 + 62400, 24000 + 67200);
    double slope = 20 * log10(e2 / e1);   /* dB per second */
    printf("decay %.1f dB/s -> RT60 %.2f s (set 2.5 s)\n", slope, -60.0 / slope);
    printf("cost: 5 s of one reverb track in %.1f ms (%.0fx real time on this machine)\n", dt * 1000, 5.0 / dt);
    FILE *f = fopen(getenv("OUT") ? getenv("OUT") : "reverb.raw", "wb"); fwrite(out, sizeof(float), (size_t)total * 2, f); fclose(f);
    return 0;
}
