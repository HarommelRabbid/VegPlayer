/* time-stretch / pitch quality benchmark for the granular engine (WSOLA "élastique" and Classic):
 *  purity:     a pure tone through each setting -> energy at the expected frequency vs the rest (dB, higher = cleaner)
 *  transients: a click train -> how much of each click's energy stays within +-1.5 ms of its peak (%, higher =
 *              sharper) and how many clicks come out doubled
 *  speed:      host ns per output frame (only useful as a ratio between versions) */
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
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

#define SR 44100
#define SEC 4
static VegTrack trk;
static VegEvent ev;
static VegMedia med;
static volatile int on[1] = { 1 };

static float *render(int16_t *src, int method, double semis, double rate, int attr, int *n_out, double *ns)
{
    VegProject *p = &g_s.proj;
    memset(p, 0, sizeof *p);
    p->ntracks = 1; p->tracks = &trk; p->nevents = 1; p->events = &ev; p->nmedia = 1; p->media = &med;
    memset(&trk, 0, sizeof trk); memset(&ev, 0, sizeof ev);
    trk.kind = VEG_KIND_AUDIO; trk.level = 1; trk.nevents = 1;
    AudioCache *ac = &g_s.m[0].ac;
    ac->ok = 1; ac->rate = SR; ac->total = SR * SEC; ac->nseg = 1;
    ac->seg[0].start = 0; ac->seg[0].count = ac->total; ac->seg[0].pcm = src;
    ev.track = 0; ev.kind = VEG_KIND_AUDIO; ev.media = 0; ev.has_take = 1; ev.level = 1;
    ev.start = 0; ev.length = 2.0; ev.rate = rate; ev.offset = 0.2; ev.has_stretch = 1; ev.stretch_method = method;
    ev.pitch_semis = semis; ev.stretch_attr = attr;
    p->duration = 2.0;
    mixer_init();
    int total = OUT_RATE * 2;
    float *out = malloc(sizeof(float) * 2 * total);
    double t0 = now();
    for (int s = 0; s < total; s += 1024) mixer_mix(out + 2 * s, total - s < 1024 ? total - s : 1024, s);
    *ns = (now() - t0) * 1e9 / total;
    *n_out = total;
    return out;
}

/* the tone's share of the signal: least-squares fit of a sinusoid (sin, cos, offset) at the best frequency near f
 * (coarse then fine search) over the middle second; fitted power vs the residual, in dB (higher = cleaner) */
static double lsres(const float *x, int a, int len, double ff, double *tot)
{
    double w = 2 * M_PI * ff / OUT_RATE;
    double Sss = 0, Scc = 0, Ssc = 0, Ss = 0, Sc = 0, Sxs = 0, Sxc = 0, Sx = 0, Sxx = 0;
    for (int i = 0; i < len; i++) {
        double sn = sin(w * i), cs = cos(w * i), v = x[2 * (a + i)];
        Sss += sn * sn; Scc += cs * cs; Ssc += sn * cs; Ss += sn; Sc += cs; Sxs += v * sn; Sxc += v * cs; Sx += v; Sxx += v * v;
    }
    /* normal equations for [s c 1] */
    double M[3][4] = { { Sss, Ssc, Ss, Sxs }, { Ssc, Scc, Sc, Sxc }, { Ss, Sc, (double)len, Sx } };
    for (int c = 0; c < 3; c++) for (int r = c + 1; r < 3; r++) { double k = M[r][c] / M[c][c]; for (int q = c; q < 4; q++) M[r][q] -= k * M[c][q]; }
    double b[3];
    for (int r = 2; r >= 0; r--) { double acc = M[r][3]; for (int q = r + 1; q < 3; q++) acc -= M[r][q] * b[q]; b[r] = acc / M[r][r]; }
    *tot = Sxx;
    return Sxx - (b[0] * Sxs + b[1] * Sxc + b[2] * Sx);   /* residual energy */
}
static double purity(const float *x, int n, double f)
{
    int a = n / 4, len = n / 2;   /* the middle: away from the event edges */
    double tot = 0, best = 1e300, bf = f;
    for (double ff = f * 0.995; ff <= f * 1.005; ff += f * 0.0002) { double r = lsres(x, a, len, ff, &tot); if (r < best) { best = r; bf = ff; } }
    double c = bf;
    for (double ff = c - f * 0.0002; ff <= c + f * 0.0002; ff += f * 0.000005) { double r = lsres(x, a, len, ff, &tot); if (r < best) best = r; }
    if (best < tot * 1e-10) best = tot * 1e-10;
    return 10 * log10((tot - best) / best);
}

/* clicks every 0.25 s of source; output click k expected near k * 0.25 / speed */
static void clicks(const float *x, int n, double speed, double *sharp, int *dup, int *found)
{
    int K = 0; double sh = 0; *dup = 0;
    for (int k = 1; ; k++) {
        if (k * 0.25 <= 0.2 * speed + 0.05) continue;   /* the event starts offset x rate into the source */
        double t = (k * 0.25 - 0.2 * speed) / speed;   /* seconds in the output */
        int c = (int)(t * OUT_RATE);
        int W = (int)(0.04 * OUT_RATE), w = (int)(0.0015 * OUT_RATE);
        if (c + W >= n) break;
        int pk = c - W; double pv = 0;
        for (int i = c - W; i < c + W; i++) { double v = fabs(x[2 * i]); if (v > pv) { pv = v; pk = i; } }
        double near = 0, all = 0;
        for (int i = c - W; i < c + W; i++) { double v = (double)x[2 * i] * x[2 * i]; all += v; if (abs(i - pk) <= w) near += v; }
        if (all > 0) { sh += near / all; K++; }
        /* doubled: another peak above half the main one, more than 3 ms away */
        for (int i = c - W; i < c + W; i++) if (abs(i - pk) > (int)(0.003 * OUT_RATE) && fabs(x[2 * i]) > 0.5 * pv) { (*dup)++; break; }
    }
    *sharp = K ? 100 * sh / K : 0;
    *found = K;
}

int main(void)
{
    g_s.m = calloc(1, sizeof(MediaRt));
    g_s.m[0].ac.seg = calloc(1, sizeof(PcmSeg));
    g_s.track_on = (volatile int *)on;
    g_s.master = 1;
    int16_t *tone = malloc(sizeof(int16_t) * 2 * SR * SEC), *clk = malloc(sizeof(int16_t) * 2 * SR * SEC);
    for (int i = 0; i < SR * SEC; i++) {
        tone[2 * i] = tone[2 * i + 1] = (int16_t)(12000 * sin(2 * M_PI * 440 * i / (double)SR));
        int c = (i % (SR / 4)) < 30 ? (int)(20000 * exp(-(i % (SR / 4)) / 6.0)) : 0;   /* a short click every 0.25 s */
        clk[2 * i] = clk[2 * i + 1] = (int16_t)(c + 600 * sin(2 * M_PI * 220 * i / (double)SR));
    }
    struct { const char *name; int method; double semis, rate; int attr; } T[] = {
        { "reference: untouched", VEG_STRETCH_NONE, 0, 1, 0 },
        { "reference: resample x1.335", VEG_STRETCH_NONE, 0, 1.3348, 0 },
        { "elastique +5 st", VEG_STRETCH_ELASTIQUE, 5, 1, 0 },
        { "elastique +12 st", VEG_STRETCH_ELASTIQUE, 12, 1, 0 },
        { "elastique -7 st", VEG_STRETCH_ELASTIQUE, -7, 1, 0 },
        { "elastique stretch 0.7x", VEG_STRETCH_ELASTIQUE, 0, 0.7, 0 },
        { "classic A03 +5 st", VEG_STRETCH_CLASSIC, 5, 1, 2 },
        { "classic A03 stretch 0.7x", VEG_STRETCH_CLASSIC, 0, 0.7, 2 },
    };
    printf("%-26s %10s %10s %8s %8s\n", "setting", "purity dB", "sharp %", "doubled", "ns/fr");
    for (unsigned t = 0; t < sizeof T / sizeof *T; t++) {
        int n; double ns, ns2;
        float *a = render(tone, T[t].method, T[t].semis, T[t].rate, T[t].attr, &n, &ns);
        if (getenv("DUMP") && atoi(getenv("DUMP")) == (int)t) { FILE *df = fopen("/tmp/claude-0/-home-claude/f259f273-fd16-5a65-bf82-a67ac40623aa/scratchpad/dump.raw", "wb"); fwrite(a, sizeof(float), 2 * (size_t)n, df); fclose(df); }
        double pur = purity(a, n, 440 * pow(2, T[t].semis / 12.0) * (T[t].method == VEG_STRETCH_NONE ? T[t].rate : 1));
        free(a);
        float *b = render(clk, T[t].method, T[t].semis, T[t].rate, T[t].attr, &n, &ns2);
        double sh; int dup, found;
        if (getenv("DUMPC") && atoi(getenv("DUMPC")) == (int)t) { FILE *df = fopen("/tmp/claude-0/-home-claude/f259f273-fd16-5a65-bf82-a67ac40623aa/scratchpad/dumpc.raw", "wb"); fwrite(b, sizeof(float), 2 * (size_t)n, df); fclose(df); }
        clicks(b, n, T[t].rate * (double)SR / SR, &sh, &dup, &found);
        free(b);
        printf("%-26s %10.1f %10.1f %5d/%-2d %8.0f\n", T[t].name, pur, sh, dup, found, (ns + ns2) / 2);
    }
    return 0;
}
