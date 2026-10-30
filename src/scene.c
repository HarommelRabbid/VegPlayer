/* scene.c - see scene.h */
#include "scene.h"
#include "common.h"
#include "media.h"
#include "mixer.h"
#include "veg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static Scene g_sc[3];
static int g_disp = -1, g_ready = -1;
static Mutex g_scm = { -1 };
static SceUID g_sema = -1;
static volatile int g_run;
static int g_thid = -1;
static float g_vx, g_vy, g_vw = SCREEN_W, g_vh = SCREEN_H;

/* CPU fallback for effects when no shader compiler is available: per scene buffer, a few scratch images */
#define FB_SLOTS 6
static GpuImage g_fb[3][FB_SLOTS];
static int g_fb_ok, g_fb_w, g_fb_h;   /* scratch capacity */

void scene_set_viewport(float x, float y, float w, float h) { g_vx = x; g_vy = y; g_vw = w; g_vh = h; }

/* ------------------------------------------------------------------ colour matrices for the effects */

static void cm_from_rows(float m[12], const float r[3][3], const float off[3])
{
    for (int i = 0; i < 3; i++) { m[i * 4] = r[i][0]; m[i * 4 + 1] = r[i][1]; m[i * 4 + 2] = r[i][2]; m[i * 4 + 3] = off[i]; }
}

static void vfx_matrix(const VegVfx *v, float m[12])
{
    cm_identity(m);
    static const float zero[3] = { 0, 0, 0 };
    switch (v->type) {
    case VFX_INVERT: {
        float a = (float)v->amount, d = 1.0f - 2.0f * a;
        float r[3][3] = { { d, 0, 0 }, { 0, d, 0 }, { 0, 0, d } }, o[3] = { a, a, a };
        cm_from_rows(m, r, o);
        break;
    }
    case VFX_GRAYSCALE: {
        float a = (float)v->amount, k = 1.0f - a;
        float r[3][3];
        const float L[3] = { 0.299f, 0.587f, 0.114f };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = a * L[j] + (i == j ? k : 0);
        cm_from_rows(m, r, zero);
        break;
    }
    case VFX_SEPIA: {
        float a = (float)v->amount, k = 1.0f - a;
        const float S[3][3] = { { 0.393f, 0.769f, 0.189f }, { 0.349f, 0.686f, 0.168f }, { 0.272f, 0.534f, 0.131f } };
        float r[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = a * S[i][j] + (i == j ? k : 0);
        cm_from_rows(m, r, zero);
        break;
    }
    case VFX_HSL: {
        float th = (float)(v->hue * 2.0 * M_PI), c = cosf(th), s = sinf(th);
        float H[3][3] = {
            { 0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s },
            { 0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s },
            { 0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s } };
        float sat = (float)v->sat, lum = (float)v->lum;
        const float L[3] = { 0.2126f, 0.7152f, 0.0722f };
        float S[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) S[i][j] = (1.0f - sat) * L[j] + (i == j ? sat : 0);
        float mh[12], ms[12], ml[12], t[12];
        cm_from_rows(mh, H, zero);
        cm_from_rows(ms, S, zero);
        float Lm[3][3] = { { lum, 0, 0 }, { 0, lum, 0 }, { 0, 0, lum } };
        cm_from_rows(ml, Lm, zero);
        cm_mul(t, ms, mh);
        cm_mul(m, ml, t);
        break;
    }
    case VFX_MATRIX:
    case VFX_COLORCORR:
        memcpy(m, v->mtx, sizeof(float) * 12);
        break;
    case VFX_LAB: {
        /* LAB Adjust, approximated in YCbCr (BT.601): lightness scale / inversion on Y, a/b offsets on Cr/Cb */
        static const float TO[3][3] = { { 0.299f, 0.587f, 0.114f }, { -0.168736f, -0.331264f, 0.5f }, { 0.5f, -0.418688f, -0.081312f } };
        static const float FROM[3][3] = { { 1, 0, 1.402f }, { 1, -0.344136f, -0.714136f }, { 1, 1.772f, 0 } };
        float to[12], from[12], mid[12], t[12];
        cm_from_rows(to, TO, zero);
        cm_from_rows(from, FROM, zero);
        float ly = (float)v->lum, kc = v->desat ? 0.0f : 1.0f;
        float D[3][3] = { { v->invert ? -ly : ly, 0, 0 }, { 0, kc, 0 }, { 0, 0, kc } };
        float o[3] = { v->invert ? 1.0f : 0.0f, (float)(v->b * 0.5), (float)(v->a * 0.5) };   /* a/b scale guessed */
        cm_from_rows(mid, D, o);
        cm_mul(t, mid, to);
        cm_mul(m, from, t);
        break;
    }
    case VFX_BRICON: {
        float k = (float)(1.0 + v->contrast), b = (float)v->bright;
        float r[3][3] = { { k, 0, 0 }, { 0, k, 0 }, { 0, 0, k } }, o[3] = { 0.5f - 0.5f * k + b, 0.5f - 0.5f * k + b, 0.5f - 0.5f * k + b };
        cm_from_rows(m, r, o);
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ gradient maps (built when playback starts) */

#define MAX_LUTS 32
typedef struct { const VegVfx *ref; vita2d_texture *tex; uint8_t rgb[256][3]; } Lut;
static Lut g_lut[MAX_LUTS];
static int g_nlut;

static void lut_fill(const VegVfx *v, uint8_t out[256][3])
{
    for (int i = 0; i < 256; i++) {
        float x = i / 255.0f, c[3];
        int k = 0;
        while (k + 1 < v->nstops && v->stop_pos[k + 1] < x) k++;
        if (v->nstops == 1 || x <= v->stop_pos[0]) { for (int j = 0; j < 3; j++) c[j] = v->stop_rgb[0][j]; }
        else if (k + 1 >= v->nstops) { for (int j = 0; j < 3; j++) c[j] = v->stop_rgb[v->nstops - 1][j]; }
        else {
            float span = v->stop_pos[k + 1] - v->stop_pos[k], u = span > 1e-6f ? (x - v->stop_pos[k]) / span : 1.0f;
            for (int j = 0; j < 3; j++) c[j] = v->stop_rgb[k][j] + (v->stop_rgb[k + 1][j] - v->stop_rgb[k][j]) * u;
        }
        for (int j = 0; j < 3; j++) out[i][j] = (uint8_t)(c[j] * 255.0f + 0.5f);
    }
}

static int same_gradient(const VegVfx *a, const VegVfx *b)
{
    if (a->nstops != b->nstops) return 0;
    for (int i = 0; i < a->nstops; i++) {
        if (fabsf(a->stop_pos[i] - b->stop_pos[i]) > 1e-4f) return 0;
        for (int j = 0; j < 3; j++) if (fabsf(a->stop_rgb[i][j] - b->stop_rgb[i][j]) > 1e-4f) return 0;
    }
    return 1;
}

static void luts_free(void)
{
    if (!g_nlut) return;
    vita2d_wait_rendering_done();
    for (int i = 0; i < g_nlut; i++) if (g_lut[i].tex) vita2d_free_texture(g_lut[i].tex);
    g_nlut = 0;
}

static void lut_assign(VegVfx *v)
{
    if (v->type != VFX_GRADIENT) return;
    for (int i = 0; i < g_nlut; i++) if (same_gradient(g_lut[i].ref, v)) { v->lut = i; return; }
    if (g_nlut >= MAX_LUTS) { v->lut = -1; return; }
    Lut *L = &g_lut[g_nlut];
    L->ref = v;
    lut_fill(v, L->rgb);
    L->tex = vita2d_create_empty_texture_format(256, 1, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (L->tex) {
        uint32_t *px = vita2d_texture_get_datap(L->tex);
        for (int i = 0; i < 256; i++) px[i] = L->rgb[i][0] | (uint32_t)L->rgb[i][1] << 8 | (uint32_t)L->rgb[i][2] << 16 | 0xFF000000u;
        vita2d_texture_set_filters(L->tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    v->lut = g_nlut++;
}

static void luts_build(void)
{
    luts_free();
    VegProject *p = &g_s.proj;
    for (int i = 0; i < p->nevents; i++) for (int k = 0; k < p->events[i].nvfx; k++) lut_assign(&p->events[i].vfx[k]);
    for (int t = 0; t < p->ntracks; t++) for (int k = 0; k < p->tracks[t].nvfx; k++) lut_assign(&p->tracks[t].vfx[k]);
    if (g_nlut) LOG("scene: %d gradient map%s", g_nlut, g_nlut > 1 ? "s" : "");
}

/* ------------------------------------------------------------------ effect chain of a layer */

/* The chain (event effects, then track effects) becomes one GPU pass: colour effects fold into the matrix m;
 * a Gradient Map or a non-linear colour effect (HSL Adjust, Threshold, Secondary Color Corrector) splits the chain
 * (m before it, m2 after it); a blur, a warp (Swirl, Wave, Deform) or Cookie Cutter is applied when sampling (they
 * commute with the colour matrix). One special effect per layer: the first one wins; a later HSL Adjust then folds
 * into the matrix as an approximation, the other special effects are left out. */
#define CHAIN_STAGES 6
typedef struct { int pass; float m[12], m2[12]; int has_sp; VegVfx sp; } Stage;
/* the last stage's fields are pass / m / m2 / special (drawn with the layer); the earlier ones (npre) are drawn into
 * render targets first, each reading the one before */
typedef struct { int pass; float m[12], m2[12]; const VegVfx *special; VegVfx sp; int npre; Stage pre[CHAIN_STAGES - 1]; } Chain;

static int special_pass(int type)
{
    switch (type) {
    case VFX_GRADIENT: return PASS_LUT;
    case VFX_LINEARBLUR: case VFX_RADIALBLUR: case VFX_GAUSSBLUR: return PASS_BLUR;
    case VFX_HSL: return PASS_HSL;
    case VFX_THRESHOLD: return PASS_THRESH;
    case VFX_SECCC: return PASS_SECCC;
    case VFX_SWIRL: return PASS_SWIRL;
    case VFX_WAVE: return PASS_WAVE;
    case VFX_DEFORM: return PASS_DEFORM;
    case VFX_COOKIE: return PASS_COOKIE;
    }
    return -1;
}

/* passes whose shader applies a second matrix after the effect */
static int pass_has_post(int pass) { return pass == PASS_LUT || pass == PASS_HSL || pass == PASS_THRESH || pass == PASS_SECCC; }

/* tau: time in the event (event effects' keyframes), T: project time (track effects'). A special effect after another
 * one starts a new stage (with the shaders; without them, or past CHAIN_STAGES, the old rule: HSL Adjust folds into the
 * matrix, the rest is left out). */
static void chain_build(const VegEvent *e, const VegTrack *t, double tau, double T, Chain *c)
{
    Stage st[CHAIN_STAGES];
    int n = 1, cur = 0, multi = gpu_shaders_ok();
    cm_identity(st[0].m); cm_identity(st[0].m2); st[0].pass = PASS_MATRIX; st[0].has_sp = 0;
    for (int pass = 0; pass < 2; pass++) {
        const VegVfx *fx = pass ? t->vfx : e->vfx;
        int nf = pass ? t->nvfx : e->nvfx;
        for (int i = 0; i < nf; i++) {
            VegVfx at;
            const VegVfx *v = veg_vfx_at(&fx[i], pass ? T : tau, &at);
            if (v->type == VFX_UNKNOWN || v->type == VFX_NOOP) continue;
            int sp = special_pass(v->type);
            if (sp >= 0) {
                int usable = (sp == PASS_LUT || sp == PASS_BLUR) ? (v->type != VFX_GRADIENT || v->lut >= 0) : gpu_pass_ok(sp);
                if (usable && !st[cur].has_sp) { st[cur].pass = sp; st[cur].sp = *v; st[cur].has_sp = 1; continue; }
                if (usable && multi && gpu_pass_ok(sp) && n < CHAIN_STAGES) {
                    cur = n++;
                    cm_identity(st[cur].m); cm_identity(st[cur].m2);
                    st[cur].pass = sp; st[cur].sp = *v; st[cur].has_sp = 1;
                    continue;
                }
                if (v->type != VFX_HSL) continue;   /* HSL Adjust: the matrix approximation below */
            }
            float f[12], r[12];
            vfx_matrix(v, f);
            float *dst = pass_has_post(st[cur].pass) ? st[cur].m2 : st[cur].m;
            cm_mul(r, f, dst);
            memcpy(dst, r, sizeof r);
        }
    }
    c->npre = n - 1;
    for (int k = 0; k < n - 1; k++) c->pre[k] = st[k];
    const Stage *L = &st[n - 1];
    c->pass = L->pass;
    memcpy(c->m, L->m, sizeof c->m); memcpy(c->m2, L->m2, sizeof c->m2);
    c->sp = L->sp;
    c->special = L->has_sp ? &c->sp : NULL;
}

/* Blurs, as Vegas applies event effects: after Pan/Crop, over the whole event frame. So the amount is measured
 * against the output frame, the direction is in output space (also for a rotated picture), and the blur spills past
 * the picture's edges into the transparent frame around it: the quad is enlarged by the blur's reach and taps that
 * land outside the picture count as transparent (shader). Called after place() and the default texcoords. */
static void blur_params(const VegVfx *v, GpuLayer *L, float frame_w, float frame_h, float frame_x, float frame_y)
{
    memset(L->dir, 0, sizeof L->dir); memset(L->rad, 0, sizeof L->rad);
    int gauss = v->type == VFX_GAUSSBLUR;
    for (int k = 0; k < 17; k++) {   /* Gaussian: binomial weights C(16, k) / 2^16; the others: a box */
        double c = 1;
        for (int j = 0; j < k; j++) c = c * (16 - j) / (j + 1);
        L->w[k] = gauss ? (float)(c / 65536.0) : 1.0f / 17.0f;
    }
    L->w[17] = L->w[18] = L->w[19] = 0;
    /* screen = P0 + u * A + v * B (the picture's quad is a parallelogram); its inverse maps screen vectors to texcoords */
    float ax = L->x[1] - L->x[0], ay = L->y[1] - L->y[0], bx = L->x[2] - L->x[0], by = L->y[2] - L->y[0];
    float det = ax * by - ay * bx;
    if (fabsf(det) < 1e-3f) return;
    float i00 = by / det, i01 = -bx / det, i10 = -ay / det, i11 = ax / det;
    float sx = 0, sy = 0;   /* screen-space step to the outermost tap */
    float ex, ey;           /* how far the quad grows, in texcoords */
    if (v->type == VFX_LINEARBLUR || v->type == VFX_GAUSSBLUR) {
        if (v->type == VFX_LINEARBLUR) {
            float a = (float)(v->angle * M_PI / 180.0), len = (float)v->amount * frame_w * 0.5f;   /* Amount: length / frame width */
            sx = cosf(a) * len; sy = -sinf(a) * len;
        } else if (v->a * frame_w >= v->b * frame_h) sx = (float)v->a * frame_w;   /* the longer axis */
        else sy = (float)v->b * frame_h;
        L->dir[0] = i00 * sx + i01 * sy;
        L->dir[1] = i10 * sx + i11 * sy;
        ex = fabsf(L->dir[0]); ey = fabsf(L->dir[1]);
    } else {
        /* Radial Blur: centre in frame coordinates -> texcoords */
        float cx = frame_x + (float)v->cx * frame_w - L->x[0], cy = frame_y + (float)v->cy * frame_h - L->y[0];
        L->dir[2] = i00 * cx + i01 * cy; L->dir[3] = i10 * cx + i11 * cy;
        L->rad[0] = (float)v->amount;
        ex = ey = (float)v->amount;
    }
    if (ex > 2) ex = 2;
    if (ey > 2) ey = 2;
    if (ex < 1e-4f && ey < 1e-4f) return;
    /* enlarge the quad: corners at texcoords (-ex | 1+ex, -ey | 1+ey) */
    float x0 = L->x[0], y0 = L->y[0];
    static const float CU[4] = { 0, 1, 0, 1 }, CV[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        float u = CU[i] ? 1 + ex : -ex, w = CV[i] ? 1 + ey : -ey;
        L->u[i] = u; L->v[i] = w;
        L->x[i] = x0 + u * ax + w * bx;
        L->y[i] = y0 + u * ay + w * by;
    }
}

/* the newer effects' shader parameters (gpu.c describes each pass); pw x ph: the picture's own pixels. All measured on
 * VEGAS renders of the effects' presets, with these exceptions: Swirl's centre offset (always 0 in the presets: taken as
 * -1..1 across the picture, y up) and the hue rotation direction of the Secondary Color Corrector (180 degrees in the
 * presets). The warps work in the picture's texture space (Vegas: the event frame; the same for unscaled pictures). */
static void fx_params(const VegVfx *v, GpuLayer *L, int pw, int ph)
{
    float *f = L->fxp;
    const float *q = v->prm;
    memset(f, 0, sizeof L->fxp);
    switch (v->type) {
    case VFX_HSL: f[0] = (float)v->hue; f[1] = (float)v->sat; f[2] = (float)v->lum; break;
    case VFX_THRESHOLD: f[0] = (float)v->amount; break;
    case VFX_SECCC: {
        float th = q[0] * (float)(M_PI / 180.0), g = q[2] > 0.01f ? q[2] : 0.01f;
        f[0] = cosf(th) * q[1]; f[1] = sinf(th) * q[1]; f[2] = 1.0f / g; f[3] = q[3];
        f[4] = q[4]; f[5] = q[5] - q[7]; f[6] = q[5]; f[7] = q[6];
        f[8] = q[6] + q[7]; f[9] = q[8] / 360.0f; f[10] = q[9] * 0.5f / 360.0f; f[11] = (q[9] * 0.5f + q[10]) / 360.0f;
        break;
    }
    case VFX_SWIRL: {
        float sx = fabsf(q[3]) > 0.001f ? q[3] : 0.001f, sy = fabsf(q[4]) > 0.001f ? q[4] : 0.001f;
        f[0] = 0.5f + 0.5f * q[1]; f[1] = 0.5f - 0.5f * q[2]; f[2] = 0.5f * sx; f[3] = 0.5f * sy;
        f[4] = -15.0f * q[0];
        break;
    }
    case VFX_WAVE:
        f[0] = (float)M_PI * q[0]; f[1] = (float)M_PI * q[1]; f[2] = q[2]; f[3] = q[3]; f[4] = q[4]; f[5] = q[5];
        break;
    case VFX_DEFORM: {
        /* edges: squeeze scales an edge's length (1 - A s), curve bows it by 0.5 A c x 4u(1-u) (the flag moves the
         * edge's ends instead of its middle), shear slants it; the inside is blended between the edges */
        float A = q[0], fl = q[1] != 0 ? 1.0f : 0.0f;
        float kT = 1 - A * q[6], kB = 1 - A * q[7], kL = 1 - A * q[8], kR = 1 - A * q[9];
        float exL = -0.5f * A * q[12] * fl, exR = -0.5f * A * q[13] * fl, eyT = 0.5f * A * q[10] * fl, eyB = 0.5f * A * q[11] * fl;
        f[0] = 0.5f - 0.5f * kT + exL; f[1] = 0.5f - 0.5f * kL + eyT;   /* top left */
        f[2] = 0.5f + 0.5f * kT + exR; f[3] = 0.5f - 0.5f * kR + eyT;   /* top right */
        f[4] = 0.5f - 0.5f * kB + exL; f[5] = 0.5f + 0.5f * kL + eyB;   /* bottom left */
        f[6] = 0.5f + 0.5f * kB + exR; f[7] = 0.5f + 0.5f * kR + eyB;   /* bottom right */
        f[8] = -0.5f * A * q[10]; f[9] = -0.5f * A * q[11]; f[10] = 0.5f * A * q[12]; f[11] = 0.5f * A * q[13];
        f[12] = A * q[4]; f[13] = A * q[5];
        break;
    }
    case VFX_COOKIE: {
        float rx = q[3], ry = q[4], rep = rx > ry ? rx : ry, half = pw * 0.5f / rep;
        int shape = v->ip[0];
        f[0] = (float)pw; f[1] = (float)ph; f[2] = rx; f[3] = ry;
        f[4] = q[5]; f[5] = 1.0f - q[6];
        f[6] = shape == 3 ? q[0] : q[0] * hypotf((float)pw, (float)ph) / rep;
        f[7] = q[2] * half;
        f[8] = 0.1465f * q[1] * half; if (f[8] < 0.4f) f[8] = 0.4f;
        f[9] = (float)shape; f[10] = (float)v->ip[1];
        f[12] = q[7]; f[13] = q[8]; f[14] = q[9];
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ placement (Pan/Crop + Track Motion) */

static void pan_at(const VegEvent *e, double tau, VegPanKey *k)
{
    if (e->npan == 1 || tau <= e->pan[0].t) { *k = e->pan[0]; return; }
    for (int i = 0; i + 1 < e->npan; i++) {
        const VegPanKey *a = &e->pan[i], *b = &e->pan[i + 1];
        if (tau <= b->t) {
            double u = b->t > a->t ? (tau - a->t) / (b->t - a->t) : 1.0;
            switch (a->curve) {   /* the keyframe's interpolation type shapes the way to the next one */
            case VEG_KEY_FAST:   u = veg_fade_shape(VEG_CURVE_FAST, u); break;
            case VEG_KEY_SLOW:   u = veg_fade_shape(VEG_CURVE_SLOW, u); break;
            case VEG_KEY_SMOOTH: u = veg_fade_shape(VEG_CURVE_SMOOTH, u); break;
            case VEG_KEY_SHARP:  u = veg_fade_shape(VEG_CURVE_SHARP, u); break;
            case VEG_KEY_HOLD:   u = 0; break;
            default: break;
            }
            k->t = tau;
            k->w = a->w + (b->w - a->w) * u; k->h = a->h + (b->h - a->h) * u;
            k->cx = a->cx + (b->cx - a->cx) * u; k->cy = a->cy + (b->cy - a->cy) * u;
            k->rot = a->rot + (b->rot - a->rot) * u;
            return;
        }
    }
    *k = e->pan[e->npan - 1];
}

/* Track Motion at project time T: between keyframes, shaped by the earlier keyframe's interpolation type (as Pan/Crop) */
static void tm_at(const VegTrack *t, double T, double *x, double *y, double *w, double *h, double *rot)
{
    if (!t->tmk || t->ntmk < 1) { *x = t->tm_x; *y = t->tm_y; *w = t->tm_w; *h = t->tm_h; *rot = 0; return; }
    const VegTmKey *k = t->tmk;
    int n = t->ntmk;
    if (n == 1 || T <= k[0].t) { *x = k[0].x; *y = k[0].y; *w = k[0].w; *h = k[0].h; *rot = k[0].rot; return; }
    int i = 0;
    while (i + 1 < n && T > k[i + 1].t) i++;
    if (i + 1 >= n) { *x = k[n - 1].x; *y = k[n - 1].y; *w = k[n - 1].w; *h = k[n - 1].h; *rot = k[n - 1].rot; return; }
    const VegTmKey *a = &k[i], *b = &k[i + 1];
    double u = b->t > a->t ? (T - a->t) / (b->t - a->t) : 1.0;
    switch (a->curve) {
    case VEG_KEY_FAST:   u = veg_fade_shape(VEG_CURVE_FAST, u); break;
    case VEG_KEY_SLOW:   u = veg_fade_shape(VEG_CURVE_SLOW, u); break;
    case VEG_KEY_SMOOTH: u = veg_fade_shape(VEG_CURVE_SMOOTH, u); break;
    case VEG_KEY_SHARP:  u = veg_fade_shape(VEG_CURVE_SHARP, u); break;
    case VEG_KEY_HOLD:   u = 0; break;
    default: break;
    }
    *x = a->x + (b->x - a->x) * u; *y = a->y + (b->y - a->y) * u;
    *w = a->w + (b->w - a->w) * u; *h = a->h + (b->h - a->h) * u;
    *rot = a->rot + (b->rot - a->rot) * u;
}

/* corners of the source picture in project-frame pixels (W x H). Vegas's model:
 * - the Pan/Crop window (centre, size, rotation, negative size = mirror) is stored in the pixels of the media the
 *   project was made with (the frame size recorded in the Pan/Crop chunk);
 * - when the source has another size, Vegas rescales the window with it, separately in x and y, so the whole
 *   layout takes the new source's shape (a 1:1 source turns a 16:9 grid into a square one);
 * - the window is fitted into the output frame keeping its shape, then Track Motion scales/moves the frame. */
static int place(const VegProject *p, const VegEvent *e, const VegTrack *t, double tau, int pw, int ph, const VegTmFx *fx,
                 float ox[4], float oy[4], float wx[4], float wy[4])
{
    double W = p->width, H = p->height;
    double px[4] = { 0, (double)pw, 0, (double)pw }, py[4] = { 0, 0, (double)ph, (double)ph };
    double qx[8], qy[8];   /* the picture's corners, then the Pan/Crop window's (what Vegas shows of the picture) */
    int nq = 4;
    double lx = 1, ly = 1;   /* how much the layout shrank/grew because the source has another shape (see below) */
    if (e->npan) {
        VegPanKey k;
        pan_at(e, tau, &k);
        double SW = e->pan_w ? e->pan_w : pw, SH = e->pan_h ? e->pan_h : ph;
        double ux = pw / SW, uy = ph / SH;   /* recorded media pixels -> this source's pixels */
        double kw = k.w * ux, kh = k.h * uy, kcx = k.cx * ux, kcy = k.cy * uy;
        double aw = fabs(kw), ah = fabs(kh);
        if (aw < 1) aw = 1;
        if (ah < 1) ah = 1;
        double kk = fmin(W / aw, H / ah);
        double c = cos(k.rot), s = sin(k.rot);
        /* the full recorded frame fitted into the output, before and after the rescale: a 16:9 layout played with
         * a 1:1 source becomes a square layout, lx = 0.5625. Track Motion moves are scaled the same way below,
         * so tracks moved by Track Motion stay on the layout's grid (Vegas doesn't do this; it's an option). */
        if (g_cfg.tm_follow_source) {
            double a0 = SW / SH, a1 = (double)pw / ph, ap = W / H;
            double w0 = a0 >= ap ? W : H * a0, h0 = a0 >= ap ? W / a0 : H;
            double w1 = a1 >= ap ? W : H * a1, h1 = a1 >= ap ? W / a1 : H;
            lx = w1 / w0; ly = h1 / h0;
        }
        /* the window is rotated by k.rot in the source, so the picture turns the other way in the output; a mirrored
         * window (negative size) flips the picture inside the rotated window, i.e. after the rotation. Both matter
         * only together: scan.veg's 90-degree mirrored boxes land on the opposite side to the plain ones. */
        for (int i = 0; i < 4; i++) {
            double dx = px[i] - kcx, dy = py[i] - kcy;
            double rx = c * dx + s * dy, ry = -s * dx + c * dy;
            if (k.w < 0) rx = -rx;
            if (k.h < 0) ry = -ry;
            qx[i] = W / 2 + kk * rx;
            qy[i] = H / 2 + kk * ry;
        }
        /* the window itself: the event frame shows only what is inside it (a window narrower than the frame leaves
         * transparent bars at the sides; Red Zone's strips are made that way) */
        double hw = aw * kk / 2, hh = ah * kk / 2;
        static const double SX[4] = { -1, 1, -1, 1 }, SY[4] = { -1, -1, 1, 1 };
        for (int i = 0; i < 4; i++) { qx[4 + i] = W / 2 + SX[i] * hw; qy[4 + i] = H / 2 + SY[i] * hh; }
        nq = 8;
    } else {
        double fit = fmin(W / pw, H / ph);
        double bw = pw * fit, bh = ph * fit, x0 = (W - bw) / 2, y0 = (H - bh) / 2;
        for (int i = 0; i < 4; i++) { qx[i] = x0 + px[i] * fit; qy[i] = y0 + py[i] * fit; }
    }
    if (fx) {   /* shadow / glow: moved and scaled inside the track's frame, before Track Motion */
        double cx = W / 2 + fx->x * H / 2 * lx, cy = H / 2 - fx->y * H / 2 * ly;
        for (int i = 0; i < nq; i++) { qx[i] = cx + (qx[i] - W / 2) * fx->w; qy[i] = cy + (qy[i] - H / 2) * fx->h; }
    }
    if (t->has_motion) {
        double asp = W / H, mx, my, mw, mh, rot;
        tm_at(t, e->start + tau, &mx, &my, &mw, &mh, &rot);   /* keyframes are on the project timeline */
        double tw = mw != 0 ? mw : asp, th = mh != 0 ? mh : 1.0;
        double sx = fabs(tw) / asp, sy = fabs(th);
        double cx = W / 2 + mx * H / 2 * lx, cy = H / 2 - my * H / 2 * ly;
        double c = cos(rot), s = sin(rot);   /* counter-clockwise on screen (y points down) */
        for (int i = 0; i < nq; i++) {
            double dx = (qx[i] - W / 2) * sx, dy = (qy[i] - H / 2) * sy;
            if (tw < 0) dx = -dx;
            if (th < 0) dy = -dy;
            qx[i] = cx + dx * c + dy * s; qy[i] = cy - dx * s + dy * c;
        }
    }
    double s = g_vw / W;
    for (int i = 0; i < 4; i++) { ox[i] = (float)(g_vx + qx[i] * s); oy[i] = (float)(g_vy + qy[i] * s); }
    if (nq < 8 || !wx) return 0;
    for (int i = 0; i < 4; i++) { wx[i] = (float)(g_vx + qx[4 + i] * s); wy[i] = (float)(g_vy + qy[4 + i] * s); }
    return 1;
}

/* ------------------------------------------------------------------ frame lookup */

static const GpuImage *frame_for(const VideoCache *vc, double src)
{
    if (!vc->ok || !vc->nimg) return NULL;
    if (vc->still) return &vc->img[0];
    if (vc->duration > 0) { src = fmod(src, vc->duration); if (src < 0) src += vc->duration; }
    int lo = 0, hi = vc->nframes - 1, k = 0;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (vc->pts[mid] <= src + 1e-4) { k = mid; lo = mid + 1; } else hi = mid - 1; }
    for (int d = 0; d < 8; d++) {   /* nearest cached frame (frames are only missing if the cache had to be thinned) */
        if (k - d >= 0 && vc->slot[k - d] >= 0) return &vc->img[vc->slot[k - d]];
        if (k + d < vc->nframes && vc->slot[k + d] >= 0) return &vc->img[vc->slot[k + d]];
    }
    return NULL;
}

/* CPU effect pass into a scratch image (only without the GPU shader) */
static const GpuImage *cpu_fx(const GpuImage *src, const float m[12], const uint8_t (*lut)[3], float lamt,
                              const float m2[12], GpuImage *dst)
{
    if (src->w > g_fb_w || src->h > g_fb_h) return src;
    GpuImage out = *dst;
    if (gpu_image_wrap(&out, dst->data, src->w, src->h, 4) < 0) return src;
    int M[12], M2[12], la = (int)(lamt * 256.0f);
    for (int i = 0; i < 12; i++) {
        M[i] = (int)(m[i] * (i % 4 == 3 ? 255.0f * 256.0f : 256.0f));
        M2[i] = (int)(m2[i] * (i % 4 == 3 ? 255.0f * 256.0f : 256.0f));
    }
    for (int y = 0; y < src->h; y++) {
        const uint8_t *row = (const uint8_t *)src->data + (size_t)y * src->stride * src->bpp;
        uint32_t *o = (uint32_t *)out.data + (size_t)y * out.stride;
        for (int x = 0; x < src->w; x++) {
            int r, g, b, a = 255;
            if (src->bpp == 2) {
                uint16_t v = ((const uint16_t *)row)[x];
                r = (v >> 11) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3;
            } else {
                uint32_t v = ((const uint32_t *)row)[x];
                r = v & 255; g = (v >> 8) & 255; b = (v >> 16) & 255; a = v >> 24;
            }
            int R = (M[0] * r + M[1] * g + M[2] * b + M[3]) >> 8;
            int G = (M[4] * r + M[5] * g + M[6] * b + M[7]) >> 8;
            int B = (M[8] * r + M[9] * g + M[10] * b + M[11]) >> 8;
            R = CLAMP(R, 0, 255); G = CLAMP(G, 0, 255); B = CLAMP(B, 0, 255);
            if (lut) {   /* Gradient Map: brightness -> colour, mixed by amount, then the effects after it */
                int l = (77 * R + 150 * G + 29 * B) >> 8;
                R += ((lut[l][0] - R) * la) >> 8; G += ((lut[l][1] - G) * la) >> 8; B += ((lut[l][2] - B) * la) >> 8;
                int R2 = (M2[0] * R + M2[1] * G + M2[2] * B + M2[3]) >> 8;
                int G2 = (M2[4] * R + M2[5] * G + M2[6] * B + M2[7]) >> 8;
                int B2 = (M2[8] * R + M2[9] * G + M2[10] * B + M2[11]) >> 8;
                R = CLAMP(R2, 0, 255); G = CLAMP(G2, 0, 255); B = CLAMP(B2, 0, 255);
            }
            o[x] = (uint32_t)R | (uint32_t)G << 8 | (uint32_t)B << 16 | (uint32_t)a << 24;
        }
    }
    *dst = out;
    return dst;
}

/* ------------------------------------------------------------------ building */

/* cut a placed layer to its Pan/Crop window (both convex, in screen units; the picture's texcoords are affine across
 * its quad, so they are interpolated along). Returns -1 when the picture is inside the window anyway, else the number
 * of pieces (0: nothing left): the clipped polygon (up to 8 corners) as a fan of quads, each in the corner order the
 * draw uses (0, 1, 2 then 1, 2, 3). */
typedef struct { float x, y, u, v; } ClipPt;
static int clip_to_window(const GpuLayer *L, const float wx[4], const float wy[4], GpuLayer out[3])
{
    static const int RING[4] = { 0, 1, 3, 2 };
    float ox[4], oy[4];
    for (int i = 0; i < 4; i++) { ox[i] = wx[RING[i]]; oy[i] = wy[RING[i]]; }
    float area = 0;
    for (int i = 0; i < 4; i++) { int j = (i + 1) & 3; area += ox[i] * oy[j] - ox[j] * oy[i]; }
    if (fabsf(area) < 1e-3f) return 0;
    float sg = area > 0 ? 1.0f : -1.0f;
    ClipPt a[12], b[12];
    int n = 4, inside_all = 1;
    for (int i = 0; i < 4; i++) { int c = RING[i]; a[i].x = L->x[c]; a[i].y = L->y[c]; a[i].u = L->u[c]; a[i].v = L->v[c]; }
    for (int e = 0; e < 4; e++) {
        float ex = ox[(e + 1) & 3] - ox[e], ey = oy[(e + 1) & 3] - oy[e];
        for (int i = 0; i < 4; i++)
            if (sg * (ex * (a[i].y - oy[e]) - ey * (a[i].x - ox[e])) < -0.01f) inside_all = 0;
    }
    if (inside_all) return -1;
    for (int e = 0; e < 4 && n > 0; e++) {
        float ex = ox[(e + 1) & 3] - ox[e], ey = oy[(e + 1) & 3] - oy[e];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const ClipPt *P = &a[i], *Q = &a[(i + 1) % n];
            float dp = sg * (ex * (P->y - oy[e]) - ey * (P->x - ox[e]));
            float dq = sg * (ex * (Q->y - oy[e]) - ey * (Q->x - ox[e]));
            if (dp >= 0) b[m++] = *P;
            if ((dp >= 0) != (dq >= 0) && m < 12) {
                float t = dp / (dp - dq);
                b[m].x = P->x + (Q->x - P->x) * t; b[m].y = P->y + (Q->y - P->y) * t;
                b[m].u = P->u + (Q->u - P->u) * t; b[m].v = P->v + (Q->v - P->v) * t;
                m++;
            }
        }
        n = m > 8 ? 8 : m;
        memcpy(a, b, sizeof(ClipPt) * (size_t)n);
    }
    if (n < 3) return 0;
    int np = 0;
    for (int i = 1; i + 1 < n && np < 3; i += 2) {
        const ClipPt *v[4] = { &a[i], &a[i + 1], &a[0], i + 2 < n ? &a[i + 2] : &a[i + 1] };
        GpuLayer *O = &out[np++];
        *O = *L;
        for (int c = 0; c < 4; c++) { O->x[c] = v[c]->x; O->y[c] = v[c]->y; O->u[c] = v[c]->u; O->v[c] = v[c]->v; }
        if (np > 1) { O->tex_prev = 1; O->nst = 0; }
    }
    return np;
}

/* a Track Motion shadow / glow layer for the picture img placed for event e: its shape, moved/scaled by fx, with edges
 * softened over the blur width, in the fx colour. Without the shadow shader: a flat, hard-edged quad of the colour. */
static void tm_fx_layer(const VegProject *p, const VegEvent *e, const VegTrack *tr, double tau, int pw, int ph,
                        const VegTmFx *fx, const GpuImage *img, float alpha, GpuLayer *L)
{
    memset(L, 0, sizeof *L);
    float wx[4], wy[4];
    int has_win = place(p, e, tr, tau, pw, ph, fx, L->x, L->y, wx, wy);
    static const float CU[4] = { 0, 1, 0, 1 }, CV[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) { L->u[i] = CU[i]; L->v[i] = CV[i]; }
    if (has_win) {
        /* the shadow of the event frame: the picture cut to its Pan/Crop window. Taken as an opaque shape (the cut
         * picture's own alpha isn't followed): the cut quad, or the window when the cut has more corners */
        GpuLayer pc[3];
        int np = clip_to_window(L, wx, wy, pc);
        if (np == 0) { L->tint[3] = 0; L->tex = &img->tex; return; }
        if (np == 1) { memcpy(L->x, pc[0].x, sizeof L->x); memcpy(L->y, pc[0].y, sizeof L->y); }
        else if (np > 1) { memcpy(L->x, wx, sizeof L->x); memcpy(L->y, wy, sizeof L->y); }
        if (np > 0 && g_s.white.data) img = &g_s.white;
    }
    cm_identity(L->m);
    cm_identity(L->m2);
    L->blend = BLEND_NORMAL;
    L->tint[0] = fx->rgba[0]; L->tint[1] = fx->rgba[1]; L->tint[2] = fx->rgba[2]; L->tint[3] = fx->rgba[3] * alpha;
    if (!gpu_pass_ok(PASS_SHADOW) || fx->blur <= 0) {
        L->pass = PASS_MATRIX;
        if (!gpu_pass_ok(PASS_SHADOW) && g_s.white.data) { L->tex = &g_s.white.tex; L->tint[3] *= 0.6f; }
        else L->tex = &img->tex;
        if (L->tex == &img->tex) { L->fx = 1; for (int r = 0; r < 3; r++) { L->m[r * 5] = 0; L->m[r * 4 + 3] = 1; } }
        return;
    }
    L->pass = PASS_SHADOW;
    L->tex = &img->tex;
    L->fx = 1;
    /* blur width (each side of the edge) in screen pixels, in the same units as the position, scaled like the track */
    double th = tr->has_motion && tr->tm_h != 0 ? fabs(tr->tm_h) : 1.0;
    float r = (float)(fx->blur * p->height / 2 * th * g_vw / p->width);
    float ax = L->x[1] - L->x[0], ay = L->y[1] - L->y[0], bx = L->x[2] - L->x[0], by = L->y[2] - L->y[0];
    float la = hypotf(ax, ay), lb = hypotf(bx, by);
    if (la < 1 || lb < 1) { L->tint[3] = 0; return; }
    float ex = r / la, ey = r / lb;
    if (ex < 1e-4f) ex = 1e-4f;
    if (ey < 1e-4f) ey = 1e-4f;
    if (ex > 4) ex = 4;
    if (ey > 4) ey = 4;
    L->soft[0] = 0.5f / ex; L->soft[1] = 0.5f / ey; L->soft[2] = ex; L->soft[3] = ey;
    float x0 = L->x[0], y0 = L->y[0];
    for (int i = 0; i < 4; i++) {
        float u = CU[i] ? 1 + ex : -ex, w = CV[i] ? 1 + ey : -ey;
        L->u[i] = u; L->v[i] = w;
        L->x[i] = x0 + u * ax + w * bx;
        L->y[i] = y0 + u * ay + w * by;
    }
}

static void build(Scene *sc, int buf, double T)
{
    VegProject *p = &g_s.proj;
    uint64_t t0 = time_us();
    sc->t = T; sc->n = 0; sc->nstage = 0; sc->cpu_fx = 0; sc->nlut = sc->nblur = sc->nshadow = 0;
    sc->vx = g_vx; sc->vy = g_vy; sc->vw = g_vw; sc->vh = g_vh;
    int fb_used = 0;
    /* Vegas: the first track is in front, so draw from the last track up */
    for (int ti = p->ntracks - 1; ti >= 0 && sc->n < MAX_LAYERS; ti--) {
        VegTrack *tr = &p->tracks[ti];
        if (tr->kind != VEG_KIND_VIDEO || !g_s.track_on[ti] || !tr->nevents) continue;
        int i = veg_track_seek(p, tr, T), end = tr->first_event + tr->nevents;
        for (; i < end && p->events[i].start <= T && sc->n < MAX_LAYERS; i++) {
            VegEvent *e = &p->events[i];
            if (e->kind != VEG_KIND_VIDEO || e->start + e->length <= T || e->media < 0) continue;
            VegMedia *vm = &p->media[e->media];
            MediaRt *m = &g_s.m[e->media];
            double tau = T - e->start;
            const GpuImage *img = NULL;
            float tint[4] = { 1, 1, 1, 1 };
            int pw = 1, ph = 1;
            if (vm->type == VEG_MEDIA_SOLID) {
                if (!g_s.white.data) continue;
                img = &g_s.white;
                tint[0] = vm->rgba[0]; tint[1] = vm->rgba[1]; tint[2] = vm->rgba[2]; tint[3] = vm->rgba[3];
                pw = p->width; ph = p->height;
            } else if (vm->type == VEG_MEDIA_FILE) {
                img = frame_for(&m->vc, (e->offset + tau) * e->rate);
                pw = m->vc.src_w; ph = m->vc.src_h;
            }
            if (!img || pw <= 0 || ph <= 0) continue;   /* text / noise generators and missing sources */
            GpuLayer *L = &sc->layer[sc->n];
            float win_x[4], win_y[4];
            int has_win = place(p, e, tr, tau, pw, ph, NULL, L->x, L->y, win_x, win_y);
            L->tex_prev = 0;
            if (vm->type == VEG_MEDIA_FILE && m->vc.imgq && g_cfg.small_copies) {
                /* drawn at <= 40 % of the cached size: sample the quarter-size copy (texture-cache friendly, no shimmer) */
                float sw = hypotf(L->x[1] - L->x[0], L->y[1] - L->y[0]), sh = hypotf(L->x[2] - L->x[0], L->y[2] - L->y[0]);
                int slot = (int)(img - m->vc.img);
                if (slot >= 0 && slot < m->vc.nimg && m->vc.imgq[slot].data &&
                    sw <= 0.4f * img->w && sh <= 0.4f * img->h) img = &m->vc.imgq[slot];
            }
            L->u[0] = 0; L->u[1] = 1; L->u[2] = 0; L->u[3] = 1;
            L->v[0] = 0; L->v[1] = 0; L->v[2] = 1; L->v[3] = 1;
            static const float L0U[4] = { 0, 1, 0, 1 }, L0V[4] = { 0, 0, 1, 1 };
            float placed_x[4], placed_y[4];
            memcpy(placed_x, L->x, sizeof placed_x); memcpy(placed_y, L->y, sizeof placed_y);
            Chain ch;
            chain_build(e, tr, tau, T, &ch);
            memcpy(L->m, ch.m, sizeof L->m);
            memcpy(L->m2, ch.m2, sizeof L->m2);
            L->pass = ch.pass;
            L->lut = NULL;
            if (ch.pass == PASS_LUT) {
                const Lut *lu = &g_lut[ch.special->lut];
                L->lut = lu->tex ? &lu->tex->gxm_tex : NULL;
                L->lutp[0] = (float)ch.special->amount; L->lutp[1] = 255.0f / 256.0f; L->lutp[2] = 0.5f / 256.0f; L->lutp[3] = 0;
            } else if (ch.pass == PASS_BLUR) {
                if (gpu_pass_ok(PASS_BLUR)) blur_params(ch.special, L, g_vw, g_vw * p->height / p->width, g_vx, g_vy);
                else L->pass = PASS_MATRIX;   /* no blur shader: draw unblurred */
            } else if (ch.pass >= PASS_HSL) fx_params(ch.special, L, pw, ph);
            L->fx = !cm_is_identity(L->m) || ch.pass != PASS_MATRIX;
            L->st0 = L->nst = 0;
            if (ch.npre && sc->nstage + ch.npre <= MAX_STAGES) {
                /* the earlier stages: each drawn over a whole render target of the picture's size (texture space) */
                L->st0 = sc->nstage; L->nst = ch.npre; L->rt_w = img->w; L->rt_h = img->h;
                if (img == &g_s.white) {
                    /* a solid colour is a 1x1 texture: its stages (a Cookie Cutter's shape, say) need a frame-sized target */
                    double k = fmin(1.0, fmin(960.0 / p->width, 544.0 / p->height));
                    L->rt_w = (int)(p->width * k + 0.5); L->rt_h = (int)(p->height * k + 0.5);
                    if (L->rt_w < 8) L->rt_w = 8;
                    if (L->rt_h < 8) L->rt_h = 8;
                }
                for (int k = 0; k < ch.npre; k++) {
                    const Stage *S = &ch.pre[k];
                    GpuLayer *G = &sc->stage[sc->nstage++];
                    memset(G, 0, sizeof *G);
                    memcpy(G->x, placed_x, sizeof G->x); memcpy(G->y, placed_y, sizeof G->y);
                    for (int c = 0; c < 4; c++) { G->u[c] = L0U[c]; G->v[c] = L0V[c]; }
                    memcpy(G->m, S->m, sizeof G->m); memcpy(G->m2, S->m2, sizeof G->m2);
                    G->pass = S->pass;
                    if (S->pass == PASS_LUT) {
                        const Lut *lu = &g_lut[S->sp.lut];
                        G->lut = lu->tex ? &lu->tex->gxm_tex : NULL;
                        if (!G->lut) G->pass = PASS_MATRIX;
                        G->lutp[0] = (float)S->sp.amount; G->lutp[1] = 255.0f / 256.0f; G->lutp[2] = 0.5f / 256.0f;
                    } else if (S->pass == PASS_BLUR) blur_params(&S->sp, G, g_vw, g_vw * p->height / p->width, g_vx, g_vy);
                    else if (S->pass >= PASS_HSL) fx_params(&S->sp, G, pw, ph);
                    /* the whole target, in screen units (the target's viewport maps them onto its pixels) */
                    G->x[0] = 0; G->x[1] = SCREEN_W; G->x[2] = 0; G->x[3] = SCREEN_W;
                    G->y[0] = 0; G->y[1] = 0; G->y[2] = SCREEN_H; G->y[3] = SCREEN_H;
                    for (int c = 0; c < 4; c++) { G->u[c] = L0U[c]; G->v[c] = L0V[c]; }
                    G->tint[0] = G->tint[1] = G->tint[2] = G->tint[3] = 1;
                    G->blend = BLEND_COPY;
                    G->fx = 1;
                    G->tex = k == 0 ? &img->tex : NULL;
                }
            }
            int dodge = 0;
            float a = tint[3] * tr->level * (g_cfg.apply_event_level ? e->level : 1.0f);
            /* fades: the event's own fade-in, or the crossfade with the event before it (whichever is longer);
             * the fade-out is left out where the next event overlaps, since that one dissolves in on top */
            double fin = e->fade_in > e->xfade_in ? e->fade_in : e->xfade_in;
            if (fin > 0 && tau < fin) a *= veg_fade_shape(e->fade_in_curve, tau / fin);
            double rem = e->length - tau;
            if (e->fade_out > 0 && e->xfade_out <= 0 && rem < e->fade_out)
                a *= 1.0f - veg_fade_shape(e->fade_out_curve, 1.0 - rem / e->fade_out);
            if (a <= 0.002f) continue;
            L->tint[0] = tint[0]; L->tint[1] = tint[1]; L->tint[2] = tint[2]; L->tint[3] = a > 1 ? 1 : a;
            L->blend = BLEND_NORMAL;
            L->tex = &img->tex;
            /* colour work the GPU can't do here goes through the CPU scratch images: everything without the shader
             * compiler, a gradient map without its shader. Blurs without their shader are skipped. */
            int cpu = L->fx && (!gpu_shaders_ok() || (L->pass == PASS_LUT && (!gpu_pass_ok(PASS_LUT) || !L->lut)));
            if (cpu) {
                if (g_fb_ok && fb_used < FB_SLOTS) {
                    const uint8_t (*lt)[3] = L->pass == PASS_LUT ? (const uint8_t (*)[3])g_lut[ch.special->lut].rgb : NULL;
                    L->tex = &cpu_fx(img, L->m, lt, (float)(ch.special && L->pass == PASS_LUT ? ch.special->amount : 0), L->m2,
                                     &g_fb[buf][fb_used++])->tex;
                    sc->cpu_fx++;
                }
                cm_identity(L->m);
                L->pass = PASS_MATRIX;
                L->fx = 0;
            }
            if (L->pass != PASS_MATRIX && L->pass != PASS_LUT && !gpu_pass_ok(L->pass)) L->pass = PASS_MATRIX;
            if (L->pass == PASS_LUT) sc->nlut++;
            else if (L->pass == PASS_BLUR) sc->nblur++;
            /* track compositing mode -> blend */
            if (g_cfg.compositing_guess) {
                float al = L->tint[3];
                switch (tr->comp_mode) {
                case VEG_COMP_ADD:      L->blend = BLEND_ADD; break;
                case VEG_COMP_SUBTRACT: L->blend = BLEND_SUBTRACT; break;
                case VEG_COMP_CUT:      L->blend = BLEND_CUT; break;
                case VEG_COMP_MULTIPLY: L->blend = BLEND_MULTIPLY; break;
                case VEG_COMP_SCREEN:   L->blend = BLEND_SCREEN; break;
                case VEG_COMP_LIGHTEN:  L->blend = BLEND_LIGHTEN; break;
                case VEG_COMP_DARKEN:   L->blend = BLEND_DARKEN; break;
                case VEG_COMP_CUSTOM:
                    /* a compositing plug-in. Red Zone's (the only one seen, on white solids for flashes and on a noise
                     * texture) acts on its VEGAS render like a colour dodge: dark stays dark while the white fades in,
                     * everything is white at full strength. A solid colour is done that way (below); others are hidden. */
                    if (vm->type != VEG_MEDIA_SOLID || !gpu_shaders_ok()) { sc->nstage = L->nst ? L->st0 : sc->nstage; continue; }
                    dodge = 1;
                    break;
                default: break;   /* Overlay, Hard Light, Dodge, Burn, Difference: need the picture below (normal) */
                }
                if (!gpu_shaders_ok() && L->blend != BLEND_ADD) L->blend = BLEND_NORMAL;   /* vita2d path: add only */
                if (L->blend == BLEND_MULTIPLY || L->blend == BLEND_SCREEN || L->blend == BLEND_LIGHTEN) {
                    for (int c = 0; c < 3; c++) L->tint[c] *= al;   /* premultiplied: the opacity mixes towards neutral */
                } else if (L->blend == BLEND_DARKEN) {
                    /* neutral for MIN is white: colour = a * c + (1 - a), folded into the last matrix */
                    float *fm = pass_has_post(L->pass) ? L->m2 : L->m;
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) fm[r * 4 + c] *= al * L->tint[r];
                        fm[r * 4 + 3] = fm[r * 4 + 3] * al * L->tint[r] + (1.0f - al);
                    }
                    L->tint[0] = L->tint[1] = L->tint[2] = 1.0f;
                    L->fx = 1;
                }
            }
            /* the event frame shows only the Pan/Crop window's part of the picture */
            GpuLayer pieces[8];
            int npieces = -1;
            if (has_win && L->pass != PASS_SHADOW) {
                npieces = clip_to_window(L, win_x, win_y, pieces);
                if (npieces == 0) { sc->nstage = L->nst ? L->st0 : sc->nstage; continue; }
                if (npieces > 0) *L = pieces[0];
            }
            if (dodge) {
                /* dodge by a constant colour c at opacity a: the picture below times 1 / (1 - a c), per channel. Done as
                 * n multiply passes of (1 + s) each (premultiplied multiply with zero alpha: dst * (1 + src)) */
                float a = L->tint[3], f[3], fmax = 1;
                for (int c = 0; c < 3; c++) {
                    float d = 1.0f - a * L->tint[c];
                    f[c] = d > 1.0f / 256 ? 1.0f / d : 256.0f;
                    if (f[c] > fmax) fmax = f[c];
                }
                if (fmax >= 32.0f) {
                    L->tint[3] = 1;   /* (nearly) full strength: the colour everywhere, black included, as VEGAS shows it */
                } else {
                    int n = (int)ceilf(log2f(fmax) - 1e-4f);
                    if (n < 1) n = 1;
                    if (n > 8) n = 8;
                    L->blend = BLEND_MULTIPLY;
                    for (int c = 0; c < 3; c++) L->tint[c] = powf(f[c], 1.0f / n) - 1.0f;
                    L->tint[3] = 0;
                    npieces = n;
                    for (int q = 1; q < n; q++) pieces[q] = *L;
                }
            }
            /* Track Motion glow / shadow: drawn just behind the layer (glow first, then shadow) */
            if (g_cfg.tm_fx && (tr->glow.on || tr->shadow.on)) {
                GpuLayer fxl[2];
                int nfx = 0;
                for (int q = 0; q < 2; q++) {
                    const VegTmFx *fx = q ? &tr->shadow : &tr->glow;
                    if (fx->on && fx->rgba[3] > 0.002f) tm_fx_layer(p, e, tr, tau, pw, ph, fx, img, a > 1 ? 1 : a, &fxl[nfx++]);
                }
                if (nfx && sc->n + nfx < MAX_LAYERS) {
                    sc->layer[sc->n + nfx] = *L;
                    for (int q = 0; q < nfx; q++) sc->layer[sc->n + q] = fxl[q];
                    sc->n += nfx;
                    sc->nshadow += nfx;
                }
            }
            sc->n++;
            for (int q = 1; q < npieces && sc->n < MAX_LAYERS; q++) sc->layer[sc->n++] = pieces[q];
        }
    }
    sc->build_us = time_us() - t0;
}

/* ------------------------------------------------------------------ effect stages (render targets) */

#define RT_MAX 24
#define RT_BUDGET (48u << 20)
static struct { vita2d_texture *t; int w, h, used; } g_rt[RT_MAX];
static int g_nrt;
static size_t g_rt_bytes;
static int g_rt_failed_logged;

static void rt_free_all(void)
{
    if (!g_nrt) return;
    vita2d_wait_rendering_done();
    for (int i = 0; i < g_nrt; i++) vita2d_free_texture(g_rt[i].t);
    g_nrt = 0; g_rt_bytes = 0; g_rt_failed_logged = 0;
}

static vita2d_texture *rt_get(int w, int h)
{
    for (int i = 0; i < g_nrt; i++) if (!g_rt[i].used && g_rt[i].w == w && g_rt[i].h == h) { g_rt[i].used = 1; return g_rt[i].t; }
    size_t bytes = (size_t)((w + 7) & ~7) * (size_t)h * 4;
    vita2d_texture *t = NULL;
    if (g_nrt < RT_MAX && g_rt_bytes + bytes <= RT_BUDGET)
        t = vita2d_create_empty_texture_rendertarget((unsigned)w, (unsigned)h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!t) {
        if (!g_rt_failed_logged) { g_rt_failed_logged = 1; LOG("scene: no memory for another %dx%d effect stage (%d in use): extra effects dropped", w, h, g_nrt); }
        return NULL;
    }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetUAddrMode(&t->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&t->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    g_rt[g_nrt].t = t; g_rt[g_nrt].w = w; g_rt[g_nrt].h = h; g_rt[g_nrt].used = 1;
    g_nrt++; g_rt_bytes += bytes;
    return t;
}

int scene_prepass(const Scene *csc)
{
    Scene *sc = (Scene *)csc;   /* the render thread owns the acquired scene */
    if (!sc || !sc->nstage) return 0;
    for (int i = 0; i < g_nrt; i++) g_rt[i].used = 0;
    gpu_own_vertices_reset();
    int drew = 0;
    for (int i = 0; i < sc->n; i++) {
        GpuLayer *L = &sc->layer[i];
        if (L->tex_prev && i > 0) { L->tex = sc->layer[i - 1].tex; continue; }
        if (!L->nst) continue;
        const SceGxmTexture *src = sc->stage[L->st0].tex;
        int ok = src != NULL;
        for (int k = 0; k < L->nst && ok; k++) {
            GpuLayer *G = &sc->stage[L->st0 + k];
            vita2d_texture *rt = rt_get(L->rt_w, L->rt_h);
            if (!rt) { ok = 0; break; }
            G->tex = src;
            vita2d_start_drawing_advanced(rt, 0);
            sceGxmSetViewport(vita2d_get_context(), L->rt_w * 0.5f, L->rt_w * 0.5f, L->rt_h * 0.5f, -L->rt_h * 0.5f, 0.5f, 0.5f);
            gpu_draw_layer_own(G);
            vita2d_end_drawing();
            src = &rt->gxm_tex;
            drew = 1;
        }
        if (ok) L->tex = src;   /* else: the picture as it is, with the last stage's effect only */
        L->nst = 0;             /* done (a scene shown twice isn't drawn again) */
    }
    return drew;
}

static int builder(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    while (g_run) {
        mutex_lock(&g_scm);
        int b = 0;
        while (b == g_disp || b == g_ready) b++;
        mutex_unlock(&g_scm);
        build(&g_sc[b], b, audio_time());
        mutex_lock(&g_scm);
        g_ready = b;
        mutex_unlock(&g_scm);
        /* wait until the renderer took a scene (or ~one frame passed) */
        SceUInt timeout = 20000;
        sceKernelWaitSema(g_sema, 1, &timeout);
    }
    return 0;
}

/* shared by playback and the offline (video export) builder: main thread */
static void scene_setup(void)
{
    if (g_scm.id < 0) mutex_init(&g_scm, "vp_scene");
    if (g_sema < 0) g_sema = sceKernelCreateSema("vp_scene_sema", 0, 0, 1, NULL);
    g_disp = g_ready = -1;
    memset(g_sc, 0, sizeof g_sc);
    luts_build();   /* gradient map textures (main thread: vita2d allocations) */
    rt_free_all();
    /* scratch images for the CPU effect fallback, sized for the largest cache frame */
    if (!gpu_shaders_ok()) {
        int mw = 0, mh = 0;
        for (int i = 0; i < g_s.proj.nmedia; i++) { mw = MAX(mw, g_s.m[i].vc.cache_w); mh = MAX(mh, g_s.m[i].vc.cache_h); }
        g_fb_ok = 0;
        if (mw > 0) {
            g_fb_ok = 1; g_fb_w = mw; g_fb_h = mh;
            for (int b = 0; b < 3 && g_fb_ok; b++)
                for (int k = 0; k < FB_SLOTS; k++)
                    if (gpu_image_alloc(&g_fb[b][k], mw, mh, 4) < 0) { g_fb_ok = 0; break; }
            LOG("scene: CPU effect fallback %s", g_fb_ok ? "ready" : "unavailable (no GPU memory left)");
        }
    }
}

int scene_start(void)
{
    scene_setup();
    g_run = 1;
    g_thid = thread_start("vp_scene", builder, NULL, 0x10000100 - 8, 64 * 1024, 0x40000);   /* core 2 */
    return g_thid >= 0 ? 0 : -1;
}

void scene_stop(void)
{
    if (g_thid < 0) return;
    g_run = 0;
    sceKernelSignalSema(g_sema, 1);
    thread_join(g_thid);
    g_thid = -1;
    g_disp = g_ready = -1;
}

const Scene *scene_acquire(void)
{
    mutex_lock(&g_scm);
    if (g_ready >= 0) { g_disp = g_ready; g_ready = -1; }
    int d = g_disp;
    mutex_unlock(&g_scm);
    sceKernelSignalSema(g_sema, 1);
    return d >= 0 ? &g_sc[d] : NULL;
}

void scene_offline_begin(void)
{
    scene_stop();
    scene_setup();
}

const Scene *scene_build_now(double T)
{
    build(&g_sc[0], 0, T);
    return &g_sc[0];
}
