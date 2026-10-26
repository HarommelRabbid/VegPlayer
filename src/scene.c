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
    case VFX_BRICON: {
        float k = (float)(1.0 + v->contrast), b = (float)v->bright;
        float r[3][3] = { { k, 0, 0 }, { 0, k, 0 }, { 0, 0, k } }, o[3] = { 0.5f - 0.5f * k + b, 0.5f - 0.5f * k + b, 0.5f - 0.5f * k + b };
        cm_from_rows(m, r, o);
        break;
    }
    default: break;
    }
}

static void chain_matrix(const VegEvent *e, const VegTrack *t, float m[12])
{
    cm_identity(m);
    for (int pass = 0; pass < 2; pass++) {
        const VegVfx *fx = pass ? t->vfx : e->vfx;
        int n = pass ? t->nvfx : e->nvfx;
        for (int i = 0; i < n; i++) {
            if (fx[i].type == VFX_UNKNOWN) continue;
            float f[12], r[12];
            vfx_matrix(&fx[i], f);
            cm_mul(r, f, m);
            memcpy(m, r, sizeof r);
        }
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
            k->t = tau;
            k->w = a->w + (b->w - a->w) * u; k->h = a->h + (b->h - a->h) * u;
            k->cx = a->cx + (b->cx - a->cx) * u; k->cy = a->cy + (b->cy - a->cy) * u;
            k->rot = a->rot + (b->rot - a->rot) * u;
            return;
        }
    }
    *k = e->pan[e->npan - 1];
}

/* corners of the source picture in project-frame pixels (W x H), following the model vegtool/tved used:
 * the picture is fitted into the Pan/Crop coordinate frame, the window (centre, size, rotation, negative size =
 * mirror) is fitted into the output frame keeping its shape, then Track Motion scales/moves the frame. */
static void place(const VegProject *p, const VegEvent *e, const VegTrack *t, double tau, int pw, int ph, float ox[4], float oy[4])
{
    double W = p->width, H = p->height;
    double px[4] = { 0, (double)pw, 0, (double)pw }, py[4] = { 0, 0, (double)ph, (double)ph };
    double qx[4], qy[4];
    if (e->npan) {
        VegPanKey k;
        pan_at(e, tau, &k);
        double SW = e->pan_w ? e->pan_w : pw, SH = e->pan_h ? e->pan_h : ph;
        double fit = fmin(SW / pw, SH / ph);
        double offx = (SW - pw * fit) / 2, offy = (SH - ph * fit) / 2;
        double aw = fabs(k.w), ah = fabs(k.h);
        if (aw < 1) aw = 1;
        if (ah < 1) ah = 1;
        double kk = fmin(W / aw, H / ah);
        double c = cos(k.rot), s = sin(k.rot);
        for (int i = 0; i < 4; i++) {
            double dx = offx + px[i] * fit - k.cx, dy = offy + py[i] * fit - k.cy;
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
        double cx = W / 2 + t->tm_x * H / 2, cy = H / 2 - t->tm_y * H / 2;
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
static const GpuImage *cpu_fx(const GpuImage *src, const float m[12], GpuImage *dst)
{
    if (src->w > g_fb_w || src->h > g_fb_h) return src;
    GpuImage out = *dst;
    if (gpu_image_wrap(&out, dst->data, src->w, src->h, 4) < 0) return src;
    int M[12];
    for (int i = 0; i < 12; i++) M[i] = (int)(m[i] * (i % 4 == 3 ? 255.0f * 256.0f : 256.0f));
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
            GpuLayer *L = &sc->layer[sc->n];
            place(p, e, tr, tau, pw, ph, L->x, L->y);
            chain_matrix(e, tr, L->m);
            L->fx = !cm_is_identity(L->m);
            float a = tint[3] * (g_cfg.apply_event_level ? e->level : 1.0f);
            if (e->xfade_in > 0 && tau < e->xfade_in) a *= (float)(tau / e->xfade_in);
            if (a <= 0.002f) continue;
            L->tint[0] = tint[0]; L->tint[1] = tint[1]; L->tint[2] = tint[2]; L->tint[3] = a > 1 ? 1 : a;
            L->blend = BLEND_NORMAL;
            if (vm->type != VEG_MEDIA_FILE && tr->has_comp && g_cfg.compositing_guess) {
                if (tr->comp_b == -1) continue;   /* plug-in compositing (masks etc.): hidden, like vegtool */
                if (tr->comp_a == 1) L->blend = BLEND_ADD;
                else if (tr->comp_a == 2) L->blend = BLEND_SUBTRACT;
            }
            L->tex = &img->tex;
            if (L->fx && !gpu_shaders_ok()) {
                if (g_fb_ok && fb_used < FB_SLOTS) {
                    L->tex = &cpu_fx(img, L->m, &g_fb[buf][fb_used++])->tex;
                    sc->cpu_fx++;
                }
                L->fx = 0;
                cm_identity(L->m);
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
