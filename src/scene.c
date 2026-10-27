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
 * a Gradient Map splits the chain (m before it, m2 after it); a blur is applied when sampling (it commutes with
 * the colour matrix). One special effect per layer: the first one wins. */
typedef struct { int pass; float m[12], m2[12]; const VegVfx *special; } Chain;

static void chain_build(const VegEvent *e, const VegTrack *t, Chain *c)
{
    cm_identity(c->m); cm_identity(c->m2);
    c->pass = PASS_MATRIX; c->special = NULL;
    for (int pass = 0; pass < 2; pass++) {
        const VegVfx *fx = pass ? t->vfx : e->vfx;
        int n = pass ? t->nvfx : e->nvfx;
        for (int i = 0; i < n; i++) {
            const VegVfx *v = &fx[i];
            if (v->type == VFX_UNKNOWN || v->type == VFX_NOOP) continue;
            if (v->type == VFX_GRADIENT || v->type == VFX_LINEARBLUR || v->type == VFX_RADIALBLUR || v->type == VFX_GAUSSBLUR) {
                if (!c->special && (v->type != VFX_GRADIENT || v->lut >= 0)) {
                    c->special = v;
                    c->pass = v->type == VFX_GRADIENT ? PASS_LUT : PASS_BLUR;
                }
                continue;
            }
            float f[12], r[12];
            vfx_matrix(v, f);
            float *dst = c->pass == PASS_LUT ? c->m2 : c->m;
            cm_mul(r, f, dst);
            memcpy(dst, r, sizeof r);
        }
    }
}

/* blur taps for the shader: step at the outermost tap (texcoords), radial centre/strength, 9 weights */
static void blur_params(const VegVfx *v, const GpuImage *img, GpuLayer *L)
{
    float asp = img->h > 0 ? (float)img->w / img->h : 1.0f;
    memset(L->dir, 0, sizeof L->dir); memset(L->rad, 0, sizeof L->rad);
    int gauss = v->type == VFX_GAUSSBLUR;
    for (int k = 0; k < 17; k++) {   /* Gaussian: binomial weights C(16, k) / 2^16; the others: a box */
        double c = 1;
        for (int j = 0; j < k; j++) c = c * (16 - j) / (j + 1);
        L->w[k] = gauss ? (float)(c / 65536.0) : 1.0f / 17.0f;
    }
    L->w[17] = L->w[18] = L->w[19] = 0;
    if (v->type == VFX_LINEARBLUR) {
        float a = (float)(v->angle * M_PI / 180.0), len = (float)v->amount * 0.5f;   /* Amount: blur length / picture width */
        L->dir[0] = cosf(a) * len; L->dir[1] = -sinf(a) * len * asp;
    } else if (v->type == VFX_GAUSSBLUR) {
        if (v->a >= v->b) L->dir[0] = (float)v->a; else L->dir[1] = (float)v->b;   /* the longer axis */
    } else {
        L->dir[2] = (float)v->cx; L->dir[3] = (float)v->cy;
        L->rad[0] = (float)v->amount;
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

/* corners of the source picture in project-frame pixels (W x H). Vegas's model:
 * - the Pan/Crop window (centre, size, rotation, negative size = mirror) is stored in the pixels of the media the
 *   project was made with (the frame size recorded in the Pan/Crop chunk);
 * - when the source has another size, Vegas rescales the window with it, separately in x and y, so the whole
 *   layout takes the new source's shape (a 1:1 source turns a 16:9 grid into a square one);
 * - the window is fitted into the output frame keeping its shape, then Track Motion scales/moves the frame. */
static void place(const VegProject *p, const VegEvent *e, const VegTrack *t, double tau, int pw, int ph, float ox[4], float oy[4])
{
    double W = p->width, H = p->height;
    double px[4] = { 0, (double)pw, 0, (double)pw }, py[4] = { 0, 0, (double)ph, (double)ph };
    double qx[4], qy[4];
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
        for (int i = 0; i < 4; i++) {
            double dx = px[i] - kcx, dy = py[i] - kcy;
            if (k.w < 0) dx = -dx;
            if (k.h < 0) dy = -dy;
            qx[i] = W / 2 + kk * (c * dx - s * dy);
            qy[i] = H / 2 + kk * (s * dx + c * dy);
        }
    } else {
        double fit = fmin(W / pw, H / ph);
        double bw = pw * fit, bh = ph * fit, x0 = (W - bw) / 2, y0 = (H - bh) / 2;
        for (int i = 0; i < 4; i++) { qx[i] = x0 + px[i] * fit; qy[i] = y0 + py[i] * fit; }
    }
    if (t->has_motion) {
        double asp = W / H;
        double tw = t->tm_w != 0 ? t->tm_w : asp, th = t->tm_h != 0 ? t->tm_h : 1.0;
        double sx = fabs(tw) / asp, sy = fabs(th);
        double cx = W / 2 + t->tm_x * H / 2 * lx, cy = H / 2 - t->tm_y * H / 2 * ly;
        for (int i = 0; i < 4; i++) {
            double dx = (qx[i] - W / 2) * sx, dy = (qy[i] - H / 2) * sy;
            if (tw < 0) dx = -dx;
            if (th < 0) dy = -dy;
            qx[i] = cx + dx; qy[i] = cy + dy;
        }
    }
    double s = g_vw / W;
    for (int i = 0; i < 4; i++) { ox[i] = (float)(g_vx + qx[i] * s); oy[i] = (float)(g_vy + qy[i] * s); }
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

static void build(Scene *sc, int buf, double T)
{
    VegProject *p = &g_s.proj;
    uint64_t t0 = time_us();
    sc->t = T; sc->n = 0; sc->cpu_fx = 0;
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
            if (tr->comp_mode == VEG_COMP_CUSTOM && g_cfg.compositing_guess) continue;   /* plug-in compositing (masks): hidden */
            GpuLayer *L = &sc->layer[sc->n];
            place(p, e, tr, tau, pw, ph, L->x, L->y);
            Chain ch;
            chain_build(e, tr, &ch);
            memcpy(L->m, ch.m, sizeof L->m);
            memcpy(L->m2, ch.m2, sizeof L->m2);
            L->pass = ch.pass;
            L->lut = NULL;
            if (ch.pass == PASS_LUT) {
                const Lut *lu = &g_lut[ch.special->lut];
                L->lut = lu->tex ? &lu->tex->gxm_tex : NULL;
                L->lutp[0] = (float)ch.special->amount; L->lutp[1] = 255.0f / 256.0f; L->lutp[2] = 0.5f / 256.0f; L->lutp[3] = 0;
            } else if (ch.pass == PASS_BLUR) blur_params(ch.special, img, L);
            L->fx = !cm_is_identity(L->m) || ch.pass != PASS_MATRIX;
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
            if (L->pass == PASS_BLUR && !gpu_pass_ok(PASS_BLUR)) L->pass = PASS_MATRIX;
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
                default: break;   /* Overlay, Hard Light, Dodge, Burn, Difference: need the picture below (normal) */
                }
                if (!gpu_shaders_ok() && L->blend != BLEND_ADD) L->blend = BLEND_NORMAL;   /* vita2d path: add only */
                if (L->blend == BLEND_MULTIPLY || L->blend == BLEND_SCREEN || L->blend == BLEND_LIGHTEN) {
                    for (int c = 0; c < 3; c++) L->tint[c] *= al;   /* premultiplied: the opacity mixes towards neutral */
                } else if (L->blend == BLEND_DARKEN) {
                    /* neutral for MIN is white: colour = a * c + (1 - a), folded into the last matrix */
                    float *fm = L->pass == PASS_LUT ? L->m2 : L->m;
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) fm[r * 4 + c] *= al * L->tint[r];
                        fm[r * 4 + 3] = fm[r * 4 + 3] * al * L->tint[r] + (1.0f - al);
                    }
                    L->tint[0] = L->tint[1] = L->tint[2] = 1.0f;
                    L->fx = 1;
                }
            }
            sc->n++;
        }
    }
    sc->build_us = time_us() - t0;
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

int scene_start(void)
{
    if (g_scm.id < 0) mutex_init(&g_scm, "vp_scene");
    if (g_sema < 0) g_sema = sceKernelCreateSema("vp_scene_sema", 0, 0, 1, NULL);
    g_disp = g_ready = -1;
    memset(g_sc, 0, sizeof g_sc);
    luts_build();   /* gradient map textures (main thread: vita2d allocations) */
    /* scratch images for the CPU effect fallback, sized for the largest cache frame */
    g_fb_ok = 0;
    if (!gpu_shaders_ok()) {
        int mw = 0, mh = 0;
        for (int i = 0; i < g_s.proj.nmedia; i++) { mw = MAX(mw, g_s.m[i].vc.cache_w); mh = MAX(mh, g_s.m[i].vc.cache_h); }
        if (mw > 0) {
            g_fb_ok = 1; g_fb_w = mw; g_fb_h = mh;
            for (int b = 0; b < 3 && g_fb_ok; b++)
                for (int k = 0; k < FB_SLOTS; k++)
                    if (gpu_image_alloc(&g_fb[b][k], mw, mh, 4) < 0) { g_fb_ok = 0; break; }
            LOG("scene: CPU effect fallback %s", g_fb_ok ? "ready" : "unavailable (no GPU memory left)");
        }
    }
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
