/* host benchmark: a chord of pitched voices (12 overlapping events) with each pitch method; prints the cost
 * relative to real time. Usage: mixbench [voices]. On a PC the numbers are only useful as ratios between methods. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "media.h"
#include "mixer.h"
Session g_s;
Config g_cfg = { -1, -1, 1, 1, 0, 1, 1, "" };
void log_printf(const char *fmt, ...) { (void)fmt; }

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv)
{
    int nv = argc > 1 ? atoi(argv[1]) : 12;
    VegProject *p = &g_s.proj;
    p->ntracks = 1; p->tracks = calloc(1, sizeof(VegTrack));
    p->nevents = nv; p->events = calloc((size_t)nv, sizeof(VegEvent));
    p->nmedia = 1; p->media = calloc(1, sizeof(VegMedia));
    g_s.m = calloc(1, sizeof(MediaRt));
    static int on[1] = { 1 };
    g_s.track_on = on;
    AudioCache *ac = &g_s.m[0].ac;
    ac->ok = 1; ac->rate = 48000; ac->total = 48000 * 10; ac->nseg = 1;
    ac->seg = calloc(1, sizeof(PcmSeg));
    ac->seg[0].start = 0; ac->seg[0].count = ac->total;
    ac->seg[0].pcm = malloc(sizeof(int16_t) * 2 * ac->total);
    srand(1);
    for (int i = 0; i < ac->total; i++) {   /* a voice-like source: harmonics with vibrato-free pitch + a little noise */
        double t = i / 48000.0, v = 0;
        for (int h = 1; h <= 8; h++) v += sin(2 * M_PI * 220 * h * t) / h;
        v = v * 0.25 + ((rand() & 1023) - 512) / 20000.0;
        ac->seg[0].pcm[2 * i] = ac->seg[0].pcm[2 * i + 1] = (int16_t)(v * 9000);
    }
    p->tracks[0].kind = VEG_KIND_AUDIO; p->tracks[0].level = 1; p->tracks[0].first_event = 0; p->tracks[0].nevents = nv;
    p->duration = 8;
    static const char *NAME[] = { "None (resample)", "Classic", "elastique (WSOLA)" };
    static const int METH[] = { VEG_STRETCH_NONE, VEG_STRETCH_CLASSIC, VEG_STRETCH_ELASTIQUE };
    double base = 0;
    for (int m = 0; m < 3; m++) {
        for (int i = 0; i < nv; i++) {
            VegEvent *e = &p->events[i];
            memset(e, 0, sizeof *e);
            e->track = 0; e->kind = VEG_KIND_AUDIO; e->media = 0; e->has_take = 1; e->level = 1.0f / nv;
            e->start = 0; e->length = 8; e->rate = 1; e->offset = 0.5 + 0.01 * i; e->seq = i;
            e->has_stretch = 1; e->stretch_method = METH[m]; e->stretch_attr = 2;
            e->pitch_semis = (double)((i * 7) % 24 - 12);   /* a spread of chord tones, -12..+11 */
            if (METH[m] == VEG_STRETCH_NONE) e->rate = pow(2.0, e->pitch_semis / 12.0);   /* resampling: pitch via rate */
        }
        mixer_init();
        int total = 48000 * 8;
        float *out = malloc(sizeof(float) * 2 * 1024);
        double t0 = now();
        for (int s = 0; s < total; s += 1024) mixer_mix(out, 1024, s);
        double dt = now() - t0;
        if (m == 0) base = dt;
        printf("%-20s %2d voices: %7.1f ms for 8 s  (%6.0fx real time, %.1fx the cost of resampling)\n", NAME[m], nv, dt * 1000, 8.0 / dt, dt / base);
        free(out);
    }
    return 0;
}
