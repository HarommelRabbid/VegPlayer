/* mixer.c - see mixer.h. The DSP part is portable; the Vita output thread is at the bottom (#ifdef __vita__). */
#include "mixer.h"
#include "common.h"
#include "media.h"
#include "veg.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VOICES 64
#define DL_LEN 4096              /* vibrato delay line, frames (85 ms) */
#define WSOLA_GRAIN 2048         /* élastique approximation: 43 ms grains, 50 % overlap */
#define WSOLA_SEARCH 480         /* +-10 ms */

/* audio speed of an event: playback rate times the Pitch Shift plug-ins that resample (and so change speed) */
double event_audio_speed(const VegEvent *e)
{
    double s = e->rate;
    for (int k = 0; k < e->nafx; k++) {
        const VegAfx *a = &e->afx[k];
        if (a->type != AFX_PITCH_SHIFT) continue;
        int preserve = g_cfg.pitch_plugin_mode >= 0 ? g_cfg.pitch_plugin_mode : a->preserve;
        if (!preserve) s *= pow(2.0, a->semitones / 12.0);
    }
    return s;
}


typedef struct {
    int used, ev, track;
    int mode;                /* 0 resample, 1 Classic granular, 2 WSOLA */
    double S, P;             /* source speed and pitch factor (1 = normal) */
    double ksrc;             /* source frames per output frame */
    double src0;             /* source frame at event time 0 */
    int64_t n;               /* next output index since the event start */
    int64_t len_n;           /* event length in output frames */
    const AudioCache *ac;
    int segc;
    /* granular */
    int L, H;
    const float *win;
    double gp[2]; int64_t gk[2];
    /* vibrato */
    int vib; double vfreq, vA, vphase;
    float *dl; int dlw;
    /* gain */
    float level;
    double fi, fo, xi, xo, len;
} Voice;

static Voice g_v[MAX_VOICES];
static float *g_dl_pool;
static int *g_next;              /* per track: next event that hasn't started */
static int g_ntracks_alloc;
static int64_t g_expect = -1;    /* sample the next mix call should start at */

/* ------------------------------------------------------------------ windows */

static struct { int L; float *w; } g_win[8];

static const float *hann(int L)
{
    for (int i = 0; i < 8; i++) if (g_win[i].L == L) return g_win[i].w;
    int slot = 0;
    for (int i = 0; i < 8; i++) if (!g_win[i].L) { slot = i; break; }
    free(g_win[slot].w);
    g_win[slot].w = malloc(sizeof(float) * (size_t)L);
    if (!g_win[slot].w) { g_win[slot].L = 0; return NULL; }
    g_win[slot].L = L;
    for (int i = 0; i < L; i++) g_win[slot].w[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / L));
    return g_win[slot].w;
}

/* ------------------------------------------------------------------ source access */

static inline void frame_at(const AudioCache *ac, int64_t i, int *segc, float *l, float *r)
{
    if (ac->total > 0 && (i >= ac->total || i < 0)) { i %= ac->total; if (i < 0) i += ac->total; }
    const PcmSeg *s = &ac->seg[*segc];
    if (i < s->start || i >= s->start + s->count) {
        int lo = 0, hi = ac->nseg - 1, f = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (ac->seg[mid].start <= i) { f = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (f < 0 || i >= ac->seg[f].start + ac->seg[f].count) { *l = *r = 0; return; }
        *segc = f;
        s = &ac->seg[f];
    }
    const int16_t *p = s->pcm + (i - s->start) * 2;
    *l = p[0] * (1.0f / 32768.0f);
    *r = p[1] * (1.0f / 32768.0f);
}

static inline void fetch_linear(Voice *v, double pos, float *l, float *r)
{
    double fl = floor(pos);
    int64_t i = (int64_t)fl;
    float t = (float)(pos - fl), l0, r0, l1, r1;
    frame_at(v->ac, i, &v->segc, &l0, &r0);
    frame_at(v->ac, i + 1, &v->segc, &l1, &r1);
    *l = l0 + (l1 - l0) * t;
    *r = r0 + (r1 - r0) * t;
}

static inline void fetch_cubic(Voice *v, double pos, float *l, float *r)
{
    double fl = floor(pos);
    int64_t i = (int64_t)fl;
    float t = (float)(pos - fl);
    float a[4], b[4];
    for (int k = 0; k < 4; k++) frame_at(v->ac, i - 1 + k, &v->segc, &a[k], &b[k]);
    float t2 = t * t, t3 = t2 * t;
    float c0 = -0.5f * t3 + t2 - 0.5f * t, c1 = 1.5f * t3 - 2.5f * t2 + 1.0f, c2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t, c3 = 0.5f * t3 - 0.5f * t2;
    *l = a[0] * c0 + a[1] * c1 + a[2] * c2 + a[3] * c3;
    *r = b[0] * c0 + b[1] * c1 + b[2] * c2 + b[3] * c3;
}

static inline float mono_at(Voice *v, double pos)
{
    float l, r;
    frame_at(v->ac, (int64_t)pos, &v->segc, &l, &r);
    return l + r;
}

/* ------------------------------------------------------------------ granular engine */

static double grain_nominal(const Voice *v, int64_t k) { return v->src0 + (double)(k * v->H) * v->ksrc * v->S; }

/* WSOLA: start of grain k near its nominal position, chosen so its first half continues grain k-1 best */
static double grain_aligned(Voice *v, int64_t k, double prev_pos)
{
    /* Classic gets a much narrower search than the élastique approximation: it keeps some of Classic's flutter */
    double nom = grain_nominal(v, k);
    double p = v->ksrc * v->P;
    double nat = prev_pos + v->H * p;
    const int D = (int)(WSOLA_SEARCH * v->ksrc * (v->mode == 2 ? 1.0 : 0.3)) + 1, DEC = 16;
    int npts = v->H / DEC;
    float y[8192 / 2 / 16 + 1];
    if (npts > (int)(sizeof y / sizeof y[0])) npts = (int)(sizeof y / sizeof y[0]);
    float ey = 0;
    for (int j = 0; j < npts; j++) { y[j] = mono_at(v, nat + (double)j * DEC * p); ey += y[j] * y[j]; }
    if (ey < 1e-6f) return nom;          /* silence: nothing to align */
    double best = nom;
    float bestc = -1e30f;
    /* coarse search every 8 source frames, then refine around the winner */
    for (int pass = 0; pass < 2; pass++) {
        double centre = best;
        int lo = pass ? -7 : -D, hi = pass ? 7 : D, step = pass ? 1 : 8;
        for (int d = lo; d <= hi; d += step) {
            double st = (pass ? centre : nom) + d;
            float c = 0, ex = 0;
            for (int j = 0; j < npts; j++) {
                float x = mono_at(v, st + (double)j * DEC * p);
                c += x * y[j];
                ex += x * x;
            }
            float score = ex > 1e-9f ? c / sqrtf(ex) : -1e30f;
            if (score > bestc) { bestc = score; best = st; }
        }
    }
    return best;
}

static void grain_prepare(Voice *v, int64_t k)
{
    if (v->gk[1] == k) return;
    if (v->gk[1] == k - 1) {
        v->gp[0] = v->gp[1]; v->gk[0] = v->gk[1];
        v->gp[1] = grain_aligned(v, k, v->gp[0]);
        v->gk[1] = k;
    } else {   /* (re)start: no history */
        v->gk[0] = k - 1; v->gp[0] = grain_nominal(v, k - 1);
        v->gk[1] = k; v->gp[1] = grain_nominal(v, k);
    }
}

static inline void granular_sample(Voice *v, int64_t n, float *l, float *r)
{
    int64_t k = n >= 0 ? n / v->H : -((-n + v->H - 1) / v->H);
    grain_prepare(v, k);
    float al = 0, ar = 0;
    double p = v->ksrc * v->P;
    for (int g = 0; g < 2; g++) {
        int64_t i = n - v->gk[g] * v->H;
        if (i < 0 || i >= v->L) continue;
        float w = v->win ? v->win[i] : 0.5f;
        float sl, sr;
        fetch_linear(v, v->gp[g] + (double)i * p, &sl, &sr);
        al += w * sl;
        ar += w * sr;
    }
    *l = al; *r = ar;
}

/* ------------------------------------------------------------------ voices */

static const int CLASSIC_MS[19] = { 80, 70, 60, 50, 40, 100, 120, 150, 30, 25, 20, 90, 110, 130, 35, 45, 55, 65, 75 };

static void voice_setup(Voice *v, int ei, double t0)
{
    VegProject *p = &g_s.proj;
    VegEvent *e = &p->events[ei];
    VegTrack *tr = &p->tracks[e->track];
    memset(v, 0, offsetof(Voice, dl));
    v->used = 1; v->ev = ei; v->track = e->track;
    v->ac = &g_s.m[e->media].ac;
    v->ksrc = (double)v->ac->rate / OUT_RATE;
    v->S = event_audio_speed(e);
    int method = g_cfg.force_stretch >= 0 ? g_cfg.force_stretch : (e->has_stretch ? e->stretch_method : VEG_STRETCH_NONE);
    double P = method == VEG_STRETCH_NONE ? e->rate : pow(2.0, e->pitch_semis / 12.0);
    int preserve_any = 0;
    for (int pass = 0; pass < 2; pass++) {
        const VegAfx *fx = pass ? tr->afx : e->afx;
        int nfx = pass ? tr->nafx : e->nafx;
        for (int k = 0; k < nfx; k++) {
            const VegAfx *a = &fx[k];
            if (a->type == AFX_PITCH_SHIFT) {
                int preserve = g_cfg.pitch_plugin_mode >= 0 ? g_cfg.pitch_plugin_mode : a->preserve;
                P *= pow(2.0, a->semitones / 12.0);
                if (preserve) preserve_any = 1;
                else if (pass) v->S *= pow(2.0, a->semitones / 12.0);   /* track-level resampling plug-in */
            } else if (a->type == AFX_VIBRATO && !v->vib) {
                v->vib = 1;
                v->vfreq = a->rate_hz > 0.05 ? a->rate_hz : 5.0;
                /* peak pitch deviation of a sine-modulated delay is A*2*pi*f (relative) */
                double dev = pow(2.0, (a->depth > 0 ? a->depth : 0.5) / 12.0) - 1.0;
                v->vA = dev / (2.0 * M_PI * v->vfreq);
                if (v->vA > 0.038) v->vA = 0.038;
            }
        }
    }
    v->P = P;
    if (fabs(v->P - v->S) < 1e-6) v->mode = 0;
    else if (method == VEG_STRETCH_ELASTIQUE && !(method == VEG_STRETCH_NONE && preserve_any)) v->mode = 2;
    else v->mode = 1;
    if (v->mode == 2) { v->L = WSOLA_GRAIN; }
    else if (v->mode == 1) {
        int a = method == VEG_STRETCH_CLASSIC ? e->stretch_attr : 4;
        int ms = CLASSIC_MS[(a < 0 ? 0 : a) % 19];
        v->L = (OUT_RATE * ms / 1000) & ~1;
    }
    if (v->mode) { v->H = v->L / 2; v->win = hann(v->L); v->gk[0] = v->gk[1] = INT64_MIN / 4; }
    v->src0 = e->offset * e->rate * v->ac->rate;
    v->len = e->length;
    v->len_n = (int64_t)llround(e->length * OUT_RATE);
    v->n = (int64_t)llround((t0 - e->start) * OUT_RATE);
    v->level = (g_cfg.apply_event_level ? e->level : 1.0f) * tr->level;   /* event gain x track volume */
    v->fi = e->fade_in; v->fo = e->fade_out; v->xi = e->xfade_in; v->xo = e->xfade_out;
    v->segc = 0;
    if (v->vib && v->dl) { memset(v->dl, 0, sizeof(float) * DL_LEN * 2); v->dlw = 0; }
}

static Voice *voice_new(int ei, double t0)
{
    VegEvent *e = &g_s.proj.events[ei];
    if (e->media < 0 || !g_s.m[e->media].ac.ok || !g_s.m[e->media].ac.nseg) return NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_v[i].used) continue;
        voice_setup(&g_v[i], ei, t0);
        return &g_v[i];
    }
    return NULL;   /* more than MAX_VOICES at once: drop */
}

static inline float ramp(double x, double len) { return len > 1e-6 && x < len ? (float)(x > 0 ? x / len : 0) : 1.0f; }

/* render into out[from..to) (block-relative), add to the mix */
static void voice_render(Voice *v, float *out, int from, int to)
{
    double inv = 1.0 / OUT_RATE;
    double p = v->ksrc * v->S;
    for (int j = from; j < to; j++, v->n++) {
        int64_t n = v->n;
        float l, r;
        if (v->mode == 0) fetch_cubic(v, v->src0 + (double)n * p, &l, &r);
        else granular_sample(v, n, &l, &r);
        if (v->vib && v->dl) {
            v->dl[v->dlw * 2] = l; v->dl[v->dlw * 2 + 1] = r;
            double d = (v->vA * (1.0 + sin(v->vphase)) + 0.001) * OUT_RATE;
            v->vphase += 2.0 * M_PI * v->vfreq * inv;
            if (v->vphase > 2.0 * M_PI) v->vphase -= 2.0 * M_PI;
            double rp = v->dlw - d;
            while (rp < 0) rp += DL_LEN;
            int i0 = (int)rp, i1 = (i0 + 1) % DL_LEN;
            float t = (float)(rp - i0);
            l = v->dl[i0 * 2] + (v->dl[i1 * 2] - v->dl[i0 * 2]) * t;
            r = v->dl[i0 * 2 + 1] + (v->dl[i1 * 2 + 1] - v->dl[i0 * 2 + 1]) * t;
            v->dlw = (v->dlw + 1) % DL_LEN;
        }
        double tau = n * inv, rem = v->len - tau;
        float g = v->level * ramp(tau, v->fi) * ramp(rem, v->fo) * ramp(tau, v->xi) * ramp(rem, v->xo);
        out[j * 2] += l * g;
        out[j * 2 + 1] += r * g;
    }
}

/* ------------------------------------------------------------------ mixing */

void mixer_init(void)
{
    if (!g_dl_pool) g_dl_pool = calloc((size_t)MAX_VOICES * DL_LEN * 2, sizeof(float));
    for (int i = 0; i < MAX_VOICES; i++) g_v[i].dl = g_dl_pool ? g_dl_pool + (size_t)i * DL_LEN * 2 : NULL;
    mixer_reset();
}

void mixer_reset(void)
{
    for (int i = 0; i < MAX_VOICES; i++) g_v[i].used = 0;
    g_expect = -1;
}

int mixer_active_voices(void)
{
    int n = 0;
    for (int i = 0; i < MAX_VOICES; i++) n += g_v[i].used;
    return n;
}

static void restart_at(double t0)
{
    VegProject *p = &g_s.proj;
    for (int i = 0; i < MAX_VOICES; i++) g_v[i].used = 0;
    if (g_ntracks_alloc < p->ntracks) {
        free(g_next);
        g_next = calloc((size_t)p->ntracks, sizeof(int));
        g_ntracks_alloc = g_next ? p->ntracks : 0;
        if (!g_next) return;
    }
    for (int t = 0; t < p->ntracks; t++) {
        VegTrack *tr = &p->tracks[t];
        int i = tr->first_event, end = tr->first_event + tr->nevents;
        for (; i < end && p->events[i].start < t0; i++) {
            VegEvent *e = &p->events[i];
            if (e->kind == VEG_KIND_AUDIO && g_s.track_on[t] && e->start + e->length > t0) voice_new(i, t0);
        }
        g_next[t] = i;
    }
}

void mixer_mix(float *out, int frames, int64_t start)
{
    memset(out, 0, sizeof(float) * (size_t)frames * 2);
    VegProject *p = &g_s.proj;
    if (!p->nevents || !g_s.m) return;
    double t0 = (double)start / OUT_RATE, t1 = (double)(start + frames) / OUT_RATE;
    if (start != g_expect || !g_next) restart_at(t0);
    g_expect = start + frames;
    if (!g_next) return;
    for (int t = 0; t < p->ntracks; t++) {
        VegTrack *tr = &p->tracks[t];
        int end = tr->first_event + tr->nevents;
        while (g_next[t] < end && p->events[g_next[t]].start < t1) {
            VegEvent *e = &p->events[g_next[t]];
            if (e->kind == VEG_KIND_AUDIO && g_s.track_on[t] && e->start + e->length > t0) voice_new(g_next[t], e->start > t0 ? e->start : t0);
            g_next[t]++;
        }
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &g_v[i];
        if (!v->used) continue;
        if (!g_s.track_on[v->track]) { v->used = 0; continue; }
        VegEvent *e = &p->events[v->ev];
        int64_t es = (int64_t)llround(e->start * OUT_RATE);
        int from = (int)(es > start ? es - start : 0);
        int64_t stop = es + v->len_n - start;
        int to = stop < frames ? (int)stop : frames;
        if (from < to) voice_render(v, out, from, to);
        if (stop <= frames) v->used = 0;
    }
}

/* ------------------------------------------------------------------ Vita output */
#ifdef __vita__
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define BLOCK 1024

static volatile int g_audio_run, g_audio_play, g_audio_thid = -1;
static Mutex g_amtx = { -1 };
/* the clock is read by the scene builder every frame: publish it with a sequence lock (64-bit values on a 32-bit CPU) */
static volatile uint32_t g_clk_seq;
static int64_t g_written;                   /* timeline sample after the last submitted block */
static uint64_t g_written_at;               /* time_us() when that block was handed over */
static volatile int64_t g_seek_to = -1;     /* written under g_amtx */
static volatile float g_seek_show = -1;     /* what audio_time() reports until the seek is processed (32-bit: atomic) */
static volatile float g_resume_hold = -1;   /* after unpausing: don't report earlier than the paused position */

static void clock_publish(int64_t written, uint64_t at)
{
    __atomic_add_fetch(&g_clk_seq, 1, __ATOMIC_SEQ_CST);
    g_written = written;
    g_written_at = at;
    __atomic_add_fetch(&g_clk_seq, 1, __ATOMIC_SEQ_CST);
}

static void clock_read(int64_t *written, uint64_t *at)
{
    for (;;) {
        uint32_t a = __atomic_load_n(&g_clk_seq, __ATOMIC_SEQ_CST);
        if (a & 1) continue;
        *written = g_written;
        *at = g_written_at;
        if (__atomic_load_n(&g_clk_seq, __ATOMIC_SEQ_CST) == a) return;
    }
}

static int audio_thread(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, BLOCK, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        LOG("audio: sceAudioOutOpenPort failed 0x%08X", port);
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, BLOCK, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    }
    if (port < 0) { g_audio_run = 0; return -1; }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    float *mix = malloc(sizeof(float) * BLOCK * 2);
    int16_t *pcm[2] = { malloc(sizeof(int16_t) * BLOCK * 2), malloc(sizeof(int16_t) * BLOCK * 2) };
    int cur = 0;
    int64_t pos = 0;
    while (g_audio_run && mix && pcm[0] && pcm[1]) {
        mutex_lock(&g_amtx);
        int seeked = 0;
        if (g_seek_to >= 0) { pos = g_seek_to; g_seek_to = -1; mixer_reset(); seeked = 1; }
        int playing = g_audio_play;
        if (playing) mixer_mix(mix, BLOCK, pos);
        mutex_unlock(&g_amtx);
        int16_t *o = pcm[cur];
        if (playing) {
            for (int i = 0; i < BLOCK * 2; i++) {
                float x = mix[i];
                /* soft knee above -3 dBFS so dense remixes don't hard-clip */
                if (x > 0.7f) x = 0.7f + 0.3f * tanhf((x - 0.7f) / 0.3f);
                else if (x < -0.7f) x = -0.7f + 0.3f * tanhf((x + 0.7f) / 0.3f);
                o[i] = (int16_t)(x * 32767.0f);
            }
            pos += BLOCK;
        } else memset(o, 0, sizeof(int16_t) * BLOCK * 2);
        sceAudioOutOutput(port, o);     /* blocks until the previous block has been taken */
        /* audio_time() reports written - 2 blocks (the blocks still queued in the port). While paused nothing of
         * the timeline is queued, so publish pos + 2 blocks to keep the clock exactly at pos: otherwise every
         * silent block after a pause/seek would show the clock 2 blocks early, and seeking to audio_time() (the
         * track panel does) would step back again each time */
        clock_publish(playing ? pos : pos + 2 * BLOCK, time_us());
        if (seeked) g_seek_show = -1;
        cur ^= 1;
    }
    sceAudioOutOutput(port, NULL);
    sceAudioOutReleasePort(port);
    free(mix); free(pcm[0]); free(pcm[1]);
    return 0;
}

int audio_start(void)
{
    if (g_audio_thid >= 0) return 0;
    if (g_amtx.id < 0) mutex_init(&g_amtx, "vp_audio");
    mixer_init();
    g_audio_run = 1; g_audio_play = 0;
    clock_publish(2 * BLOCK, time_us());
    g_audio_thid = thread_start("vp_audio", audio_thread, NULL, 64, 128 * 1024, 0x20000);   /* high priority, core 1 */
    return g_audio_thid >= 0 ? 0 : -1;
}

void audio_stop(void)
{
    if (g_audio_thid < 0) return;
    g_audio_run = 0;
    thread_join(g_audio_thid);
    g_audio_thid = -1;
}

void audio_play(int on)
{
    if (!on && g_audio_play) {   /* freeze the clock where we are */
        double t = audio_time();
        g_audio_play = 0;
        audio_seek(t);
        return;
    }
    if (on && !g_audio_play) {
        /* the first new block is heard after the silent one still queued: hold the clock until it catches up,
         * so resuming never steps the picture back */
        g_resume_hold = (float)audio_time();
    }
    g_audio_play = on;
}
int audio_playing(void) { return g_audio_play; }

void audio_seek(double s)
{
    if (s < 0) s = 0;
    mutex_lock(&g_amtx);
    g_seek_to = (int64_t)(s * OUT_RATE);
    g_seek_show = (float)s;   /* so audio_time() jumps right away */
    g_resume_hold = -1;
    mutex_unlock(&g_amtx);
}

double audio_time(void)
{
    float ss = g_seek_show;
    if (ss >= 0) return ss;
    int64_t written;
    uint64_t at;
    clock_read(&written, &at);
    /* the block just handed over starts playing when the one before it ends: heard position = written - 2 blocks */
    double base = (double)(written - 2 * BLOCK) / OUT_RATE;
    if (g_audio_play) {
        double dt = (double)(time_us() - at) / 1e6;
        if (dt > (double)BLOCK / OUT_RATE) dt = (double)BLOCK / OUT_RATE;
        base += dt;
        float hold = g_resume_hold;
        if (hold >= 0) {
            if (base < hold) return hold;
            g_resume_hold = -1;
        }
    }
    return base < 0 ? 0 : base;
}
#endif
