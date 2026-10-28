/* sharptext.c - see sharptext.h */
#include "sharptext.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/pgf.h>
#include <psp2/pvf.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_W      512
#define PAGE_H      512
#define MAX_PAGES   8          /* 2 MB of glyphs before the cache starts over */
#define MAX_FONTS   8
#define PAD         1          /* empty texels around each glyph in the atlas */
#define PVF_BASE    10.125f    /* vita2d's PVF char size at scale 1.0 */
#define SCREEN_DPI  220.0f     /* vita2d's PGF line height constant */

enum { K_PGF, K_PVF };

typedef struct {
    uint64_t key;              /* 0 = empty slot */
    int16_t page; uint16_t x, y, w, h;
    int16_t left, top;         /* bitmap offset from the pen (x right, top = rows above the baseline) */
    float adv;                 /* pen advance in pixels */
} Glyph;

typedef struct { int y, h, x; } Shelf;

typedef struct {
    vita2d_texture *tex;
    Shelf shelf[64]; int nshelf; int next_y;
} Page;

typedef struct {
    void *handle;              /* SceFontHandle or ScePvfFontId */
    int (*in_group)(unsigned int c);
    float cur_size;            /* PVF: char size currently set */
} Face;

struct stext_font {
    int kind;
    void *lib;                 /* SceFontLibHandle or ScePvfLibId */
    Face face[MAX_FONTS]; int nface;
    float vsize;               /* line advance at scale 1.0 */
    Page *page[MAX_PAGES]; int npage;
    vita2d_texture *retired[MAX_PAGES * 4]; int nretired; SceUInt64 retired_at;
    Glyph *tab; unsigned int tabcap, tabn;
    uint8_t *scratch; size_t scratchsz;
    float *fscratch; size_t fscratchsz;
    SceKernelLwMutexWork mutex;
};

/* ------------------------------------------------------------------ small helpers */

static void *lib_alloc(void *u, unsigned int size) { (void)u; return memalign(sizeof(int), (size + sizeof(int) - 1) / sizeof(int) * sizeof(int)); }
static void *lib_realloc(void *u, void *p, unsigned int size) { (void)u; return realloc(p, (size + sizeof(int) - 1) / sizeof(int) * sizeof(int)); }
static void lib_free(void *u, void *p) { (void)u; free(p); }

static int utf8_next(const char *s, unsigned int *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) { *cp = (u[0] & 0x1Fu) << 6 | (u[1] & 0x3Fu); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = (u[0] & 0x0Fu) << 12 | (u[1] & 0x3Fu) << 6 | (u[2] & 0x3Fu); return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = (u[0] & 0x07u) << 18 | (u[1] & 0x3Fu) << 12 | (u[2] & 0x3Fu) << 6 | (u[3] & 0x3Fu); return 4;
    }
    *cp = 0xFFFD; return 1;
}

static int floor64(int v) { return v >= 0 ? v / 64 : -((-v + 63) / 64); }
static int ceil64(int v) { return -floor64(-v); }

static uint8_t *scratch(stext_font *f, size_t n)
{
    if (n > f->scratchsz) {
        free(f->scratch);
        f->scratch = malloc(n);
        f->scratchsz = f->scratch ? n : 0;
    }
    if (f->scratch) memset(f->scratch, 0, n);
    return f->scratch;
}

static float *fscratch(stext_font *f, size_t n)
{
    if (n > f->fscratchsz) {
        free(f->fscratch);
        f->fscratch = malloc(n * sizeof(float));
        f->fscratchsz = f->fscratch ? n : 0;
    }
    return f->fscratch;
}

/* ------------------------------------------------------------------ glyph table (open addressing) */

static Glyph *tab_find(stext_font *f, uint64_t key, int insert)
{
    if (insert && (f->tabn + 1) * 4 > f->tabcap * 3) {
        unsigned int ncap = f->tabcap ? f->tabcap * 2 : 512;
        Glyph *nt = calloc(ncap, sizeof(Glyph));
        if (!nt) return NULL;
        for (unsigned int i = 0; i < f->tabcap; i++) {
            if (!f->tab[i].key) continue;
            unsigned int h = (unsigned int)((f->tab[i].key * 0x9E3779B97F4A7C15ull) >> 40) & (ncap - 1);
            while (nt[h].key) h = (h + 1) & (ncap - 1);
            nt[h] = f->tab[i];
        }
        free(f->tab);
        f->tab = nt; f->tabcap = ncap;
    }
    if (!f->tabcap) return NULL;
    unsigned int h = (unsigned int)((key * 0x9E3779B97F4A7C15ull) >> 40) & (f->tabcap - 1);
    while (f->tab[h].key) {
        if (f->tab[h].key == key) return &f->tab[h];
        h = (h + 1) & (f->tabcap - 1);
    }
    if (!insert) return NULL;
    memset(&f->tab[h], 0, sizeof(Glyph));
    f->tab[h].key = key;
    f->tabn++;
    return &f->tab[h];
}

/* ------------------------------------------------------------------ atlas */

static void cache_reset(stext_font *f)
{
    /* the GPU may still be drawing this frame from the old pages: keep them a little before freeing */
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (f->nretired && now - f->retired_at > 250000) {
        for (int i = 0; i < f->nretired; i++) vita2d_free_texture(f->retired[i]);
        f->nretired = 0;
    }
    for (int i = 0; i < f->npage; i++) {
        if (f->nretired < (int)(sizeof f->retired / sizeof f->retired[0])) f->retired[f->nretired++] = f->page[i]->tex;
        else vita2d_free_texture(f->page[i]->tex);   /* only reachable when flushing many times a frame */
        free(f->page[i]);
    }
    f->retired_at = now;
    f->npage = 0;
    if (f->tab) memset(f->tab, 0, f->tabcap * sizeof(Glyph));
    f->tabn = 0;
}

static Page *page_new(stext_font *f)
{
    if (f->npage >= MAX_PAGES) return NULL;
    Page *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->tex = vita2d_create_empty_texture_format(PAGE_W, PAGE_H, SCE_GXM_TEXTURE_FORMAT_U8_R111);
    if (!p->tex) { free(p); return NULL; }
    memset(vita2d_texture_get_datap(p->tex), 0, (size_t)vita2d_texture_get_stride(p->tex) * PAGE_H);
    vita2d_texture_set_filters(p->tex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);   /* drawn 1:1 */
    f->page[f->npage++] = p;
    return p;
}

/* find room for a w x h box: a shelf of about the same height, else a new shelf, else a new page */
static int atlas_place(stext_font *f, int w, int h, int *pg, int *x, int *y)
{
    w += PAD; h += PAD;
    if (w > PAGE_W || h > PAGE_H) return 0;
    for (int i = 0; i < f->npage; i++) {
        Page *p = f->page[i];
        for (int s = 0; s < p->nshelf; s++) {
            Shelf *sh = &p->shelf[s];
            if (sh->h >= h && sh->h <= h + h / 4 + 2 && sh->x + w <= PAGE_W) {
                *pg = i; *x = sh->x; *y = sh->y; sh->x += w; return 1;
            }
        }
    }
    for (int tries = 0; tries < 2; tries++) {
        Page *p = f->npage ? f->page[f->npage - 1] : NULL;
        if (p && p->nshelf < 64 && p->next_y + h <= PAGE_H) {
            Shelf *sh = &p->shelf[p->nshelf++];
            sh->y = p->next_y; sh->h = h; sh->x = w;
            p->next_y += h;
            *pg = f->npage - 1; *x = 0; *y = sh->y;
            return 1;
        }
        if (!page_new(f)) return -1;   /* full: caller resets the cache */
    }
    return 0;
}

/* copy an 8-bit image into the atlas and fill in the glyph's place */
static int atlas_store(stext_font *f, Glyph *g, const uint8_t *img, int w, int h, int pitch)
{
    if (w <= 0 || h <= 0) { g->page = -1; g->w = g->h = 0; return 1; }
    int pg, x, y, r = atlas_place(f, w, h, &pg, &x, &y);
    if (r <= 0) return r;
    vita2d_texture *t = f->page[pg]->tex;
    uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(t);
    unsigned int stride = vita2d_texture_get_stride(t);
    for (int j = 0; j < h; j++) memcpy(dst + (size_t)(y + j) * stride + x, img + (size_t)j * pitch, (size_t)w);
    g->page = (int16_t)pg; g->x = (uint16_t)x; g->y = (uint16_t)y; g->w = (uint16_t)w; g->h = (uint16_t)h;
    return 1;
}

/* trim empty borders of an 8-bit image; adjusts the bitmap offset */
static void trim(const uint8_t *img, int pitch, int *x0, int *y0, int *w, int *h)
{
    int l = 0, r = *w - 1, t = 0, b = *h - 1;
    #define ROW_EMPTY(j) ({ int e_ = 1; for (int i_ = l; i_ <= r; i_++) if (img[(size_t)(j) * pitch + i_]) { e_ = 0; break; } e_; })
    #define COL_EMPTY(i) ({ int e_ = 1; for (int j_ = t; j_ <= b; j_++) if (img[(size_t)j_ * pitch + (i)]) { e_ = 0; break; } e_; })
    while (t <= b && ROW_EMPTY(t)) t++;
    while (b >= t && ROW_EMPTY(b)) b--;
    if (t > b) { *w = *h = 0; return; }
    while (l <= r && COL_EMPTY(l)) l++;
    while (r >= l && COL_EMPTY(r)) r--;
    #undef ROW_EMPTY
    #undef COL_EMPTY
    *x0 = l; *y0 = t; *w = r - l + 1; *h = b - t + 1;
}

/* ------------------------------------------------------------------ resampling (PGF) */

/* weights mapping source samples (bitmap starting at src0, n wide) onto output pixels starting at dst0;
 * s = output pixels per source pixel. Fills idx/wt (taps per output), returns taps per output pixel. */
static int weights(int n, int src0, int dst0, int nout, double s, int *idx, float *wt, int maxtaps)
{
    int taps = s < 1 ? (int)ceil(1.0 / s) + 1 : 4;
    if (taps > maxtaps) taps = maxtaps;
    for (int u = 0; u < nout; u++) {
        int *I = idx + u * taps; float *W = wt + u * taps;
        if (s < 1) {   /* area average: exact coverage of the output pixel */
            double a = (dst0 + u) / s - src0, b = (dst0 + u + 1) / s - src0;
            int i0 = (int)floor(a);
            for (int k = 0; k < taps; k++) {
                int i = i0 + k;
                double lo = a > i ? a : i, hi = b < i + 1 ? b : i + 1;
                double w = hi > lo ? hi - lo : 0;
                if (i < 0 || i >= n) w = 0;
                I[k] = i < 0 ? 0 : i >= n ? n - 1 : i; W[k] = (float)w;
            }
            for (int k = 0; k < taps; k++) W[k] = (float)(W[k] * s);   /* coverage of a full pixel = 1 */
            continue;
        }
        double c = (dst0 + u + 0.5) / s - src0 - 0.5;   /* Catmull-Rom at the output pixel's centre */
        int i0 = (int)floor(c) - 1;
        double t = c - floor(c);
        double wk[4] = { ((-0.5 * t + 1.0) * t - 0.5) * t, (1.5 * t - 2.5) * t * t + 1.0,
                         ((-1.5 * t + 2.0) * t + 0.5) * t, (0.5 * t - 0.5) * t * t };
        for (int k = 0; k < 4; k++) {
            int i = i0 + k;
            I[k] = i < 0 ? 0 : i >= n ? n - 1 : i;
            W[k] = (i < 0 || i >= n) ? 0.0f : (float)wk[k];
        }
    }
    return taps;
}

static uint8_t *resample(stext_font *f, const uint8_t *src, int w, int h, int left, int top, double s,
                         int *ow, int *oh, int *oleft, int *otop)
{
    /* the bitmap covers x in [left, left+w), y in [-top, -top+h) around the pen; map that through the scale */
    int x0 = (int)floor(left * s), x1 = (int)ceil((left + w) * s);
    int y0 = (int)floor(-top * s), y1 = (int)ceil((-top + h) * s);
    if (s > 1) { x0--; x1++; y0--; y1++; }   /* bicubic overshoot */
    int W = x1 - x0, H = y1 - y0;
    if (W <= 0 || H <= 0) { *ow = *oh = 0; return NULL; }
    int maxt = s < 1 ? (int)ceil(1.0 / s) + 1 : 4;
    size_t need = (size_t)(W + H) * maxt;
    int *idx = malloc(need * sizeof(int));
    float *wt = malloc(need * sizeof(float));
    float *mid = fscratch(f, (size_t)W * h);
    uint8_t *out = mid && idx && wt ? scratch(f, (size_t)W * H) : NULL;
    if (!out) { free(idx); free(wt); *ow = *oh = 0; return NULL; }
    int tx = weights(w, left, x0, W, s, idx, wt, maxt);
    int ty = weights(h, -top, y0, H, s, idx + (size_t)W * maxt, wt + (size_t)W * maxt, maxt);
    for (int j = 0; j < h; j++)
        for (int u = 0; u < W; u++) {
            const int *I = idx + u * tx; const float *K = wt + u * tx;
            float a = 0;
            for (int k = 0; k < tx; k++) a += K[k] * src[(size_t)j * w + I[k]];
            mid[(size_t)j * W + u] = a;
        }
    const int *IY = idx + (size_t)W * maxt; const float *KY = wt + (size_t)W * maxt;
    for (int v = 0; v < H; v++)
        for (int u = 0; u < W; u++) {
            float a = 0;
            for (int k = 0; k < ty; k++) a += KY[v * ty + k] * mid[(size_t)IY[v * ty + k] * W + u];
            int q = (int)(a + 0.5f);
            out[(size_t)v * W + u] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
        }
    free(idx); free(wt);
    *ow = W; *oh = H; *oleft = x0; *otop = -y0;
    return out;
}

/* ------------------------------------------------------------------ glyph creation */

static int face_for(stext_font *f, unsigned int c)
{
    for (int i = 0; i < f->nface; i++)
        if (!f->face[i].in_group || f->face[i].in_group(c)) return i;
    return 0;
}

static void pvf_size(Face *fc, float size)
{
    if (fc->cur_size != size) { scePvfSetCharSize(fc->handle, size, size); fc->cur_size = size; }
}

static int make_pvf(stext_font *f, Glyph *g, int fi, unsigned int c, double s)
{
    Face *fc = &f->face[fi];
    pvf_size(fc, (float)(PVF_BASE * s));
    ScePvfCharInfo ci; ScePvfIrect rc;
    if (scePvfGetCharInfo(fc->handle, (ScePvfCharCode)c, &ci) < 0) return 0;
    if (scePvfGetCharImageRect(fc->handle, (ScePvfCharCode)c, &rc) < 0) return 0;
    g->adv = ci.glyphMetrics.horizontalAdvance64 / 64.0f;
    const int M = 1;
    int ix = floor64(ci.glyphMetrics.horizontalBearingX64), iy = ceil64(ci.glyphMetrics.horizontalBearingY64);
    int bw = rc.width + 2 * M + 1, bh = rc.height + 2 * M + 1;
    if (rc.width == 0 || rc.height == 0) { g->page = -1; return 1; }
    uint8_t *buf = scratch(f, (size_t)bw * bh);
    if (!buf) return 0;
    ScePvfUserImageBufferRec im;
    memset(&im, 0, sizeof im);
    im.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
    im.xPos64 = (M - ix) * 64;            /* pen on a whole pixel; the outline keeps its sub-pixel offset */
    im.yPos64 = (M + iy) * 64;
    im.rect.width = (ScePvfU16)bw; im.rect.height = (ScePvfU16)bh;
    im.bytesPerLine = (ScePvfU16)bw;
    im.buffer = buf;
    if (scePvfGetCharGlyphImage(fc->handle, (ScePvfCharCode)c, &im) < 0) return 0;
    int tx = 0, ty = 0, w = bw, h = bh;
    trim(buf, bw, &tx, &ty, &w, &h);
    g->left = (int16_t)(ix - M + tx);
    g->top = (int16_t)(M + iy - ty);
    return atlas_store(f, g, buf + (size_t)ty * bw + tx, w, h, bw);
}

static int make_pgf(stext_font *f, Glyph *g, int fi, unsigned int c, double s)
{
    Face *fc = &f->face[fi];
    SceFontCharInfo ci;
    if (sceFontGetCharInfo(fc->handle, c, &ci) < 0) return 0;
    int w = (int)ci.bitmapWidth, h = (int)ci.bitmapHeight, left = (int)ci.bitmapLeft, top = (int)ci.bitmapTop;
    g->adv = (float)(ci.sfp26AdvanceH / 64.0 * s);
    if (w <= 0 || h <= 0) { g->page = -1; return 1; }
    uint8_t *src = malloc((size_t)w * h);
    if (!src) return 0;
    memset(src, 0, (size_t)w * h);
    SceFontGlyphImage im;
    memset(&im, 0, sizeof im);
    im.pixelFormat = SCE_FONT_PIXELFORMAT_8;
    im.bufWidth = (unsigned short)w; im.bufHeight = (unsigned short)h; im.bytesPerLine = (unsigned short)w;
    im.bufferPtr = (unsigned int)(uintptr_t)src;
    if (sceFontGetCharGlyphImage(fc->handle, c, &im) != 0) { free(src); return 0; }
    int r;
    if (fabs(s - 1.0) < 1e-6) {
        g->left = (int16_t)left; g->top = (int16_t)top;
        r = atlas_store(f, g, src, w, h, w);
    } else {
        int ow, oh, ol, ot;
        uint8_t *img = resample(f, src, w, h, left, top, s, &ow, &oh, &ol, &ot);
        if (!img) { free(src); return ow == 0 ? (g->page = -1, 1) : 0; }
        int tx = 0, ty = 0, tw = ow, th = oh;
        trim(img, ow, &tx, &ty, &tw, &th);
        g->left = (int16_t)(ol + tx); g->top = (int16_t)(ot - ty);
        r = atlas_store(f, g, img + (size_t)ty * ow + tx, tw, th, ow);
    }
    free(src);
    return r;
}

/* flip a stored glyph left-right within its advance */
static void mirror_glyph(stext_font *f, Glyph *g)
{
    if (g->page < 0 || !g->w) return;
    vita2d_texture *t = f->page[g->page]->tex;
    uint8_t *d = (uint8_t *)vita2d_texture_get_datap(t);
    unsigned int stride = vita2d_texture_get_stride(t);
    for (int j = 0; j < g->h; j++) {
        uint8_t *row = d + (size_t)(g->y + j) * stride + g->x;
        for (int a = 0, b = g->w - 1; a < b; a++, b--) { uint8_t tmp = row[a]; row[a] = row[b]; row[b] = tmp; }
    }
    g->left = (int16_t)lroundf(g->adv - g->left - g->w);
}

/* cached glyph for (char, size); NULL if the font doesn't have it */
static const Glyph *glyph(stext_font *f, unsigned int c, int q, int *fi_out)
{
    int fi = face_for(f, c);
    *fi_out = fi;
    uint64_t key = (uint64_t)(c & 0x1FFFFF) | (uint64_t)(fi + 1) << 21 | (uint64_t)q << 32;
    Glyph *g = tab_find(f, key, 0);
    if (g) return g->adv == 0 && g->page == -2 ? NULL : g;
    for (int attempt = 0; attempt < 2; attempt++) {
        g = tab_find(f, key, 1);
        if (!g) return NULL;
        double s = q / 64.0;
        /* the system fonts draw U+005C as a yen sign (Japanese code page heritage): make the backslash from the
         * slash, mirrored */
        unsigned int src = c == '\\' ? '/' : c;
        int r = f->kind == K_PVF ? make_pvf(f, g, fi, src, s) : make_pgf(f, g, fi, src, s);
        if (r > 0 && src != c) mirror_glyph(f, g);
        if (r > 0) return g;
        if (r < 0 && attempt == 0) { cache_reset(f); continue; }   /* atlas full: start over, retry once */
        g->page = -2; g->adv = 0;   /* missing glyph: remember it */
        return NULL;
    }
    return NULL;
}

/* ------------------------------------------------------------------ drawing / measuring */

static int run(stext_font *f, int draw, int *height, int x, int y, float linespace, unsigned int color,
               float scale, const char *text)
{
    if (!f || !text) return 0;
    sceKernelLockLwMutex(&f->mutex, 1, NULL);
    int q = (int)lroundf(scale * 64.0f);
    if (q < 1) q = 1;
    if (q > 0xFFFF) q = 0xFFFF;
    double s = q / 64.0;
    double pen_x = x, max_x = x;
    int pen_y = y;
    unsigned int prev = 0; int prev_fi = -1;
    for (int i = 0; text[i];) {
        unsigned int c;
        i += utf8_next(text + i, &c);
        if (c == '\n') {
            if (pen_x > max_x) max_x = pen_x;
            pen_x = x;
            pen_y += (int)lround(f->vsize * s + linespace);
            prev = 0;
            continue;
        }
        int fi;
        const Glyph *g = glyph(f, c, q, &fi);
        if (!g) { prev = 0; continue; }
        if (f->kind == K_PVF && prev && prev_fi == fi) {
            ScePvfKerningInfo k;
            pvf_size(&f->face[fi], (float)(PVF_BASE * s));
            if (scePvfGetKerningInfo(f->face[fi].handle, (ScePvfCharCode)prev, (ScePvfCharCode)c, &k) >= 0)
                pen_x += k.fKerningInfo.xOffset;
        }
        if (draw && g->page >= 0 && g->page < f->npage)
            vita2d_draw_texture_tint_part(f->page[g->page]->tex, (float)(lround(pen_x) + g->left), (float)(pen_y - g->top),
                                          g->x, g->y, g->w, g->h, color);
        pen_x += g->adv;
        prev = c; prev_fi = fi;
    }
    if (pen_x > max_x) max_x = pen_x;
    if (height) *height = pen_y + (int)lround(f->vsize * s) - y;
    sceKernelUnlockLwMutex(&f->mutex, 1);
    return (int)ceil(max_x) - x;
}

int stext_draw_text(stext_font *f, int x, int y, unsigned int color, float scale, const char *text)
{
    return run(f, 1, NULL, x, y, 0, color, scale, text);
}

int stext_draw_text_ls(stext_font *f, int x, int y, float linespace, unsigned int color, float scale, const char *text)
{
    return run(f, 1, NULL, x, y, linespace, color, scale, text);
}

int stext_draw_textf(stext_font *f, int x, int y, unsigned int color, float scale, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return run(f, 1, NULL, x, y, 0, color, scale, buf);
}

void stext_text_dimensions(stext_font *f, float scale, const char *text, int *width, int *height)
{
    int w = run(f, 0, height, 0, 0, 0, 0, scale, text);
    if (width) *width = w;
}

int stext_text_width(stext_font *f, float scale, const char *text) { return run(f, 0, NULL, 0, 0, 0, 0, scale, text); }

int stext_text_height(stext_font *f, float scale, const char *text)
{
    int h = 0;
    run(f, 0, &h, 0, 0, 0, 0, scale, text);
    return h;
}

void stext_flush(stext_font *f)
{
    if (!f) return;
    sceKernelLockLwMutex(&f->mutex, 1, NULL);
    cache_reset(f);
    sceKernelUnlockLwMutex(&f->mutex, 1);
}

/* ------------------------------------------------------------------ loading */

static stext_font *font_new(int kind)
{
    stext_font *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->kind = kind;
    sceKernelCreateLwMutex(&f->mutex, "stext_mutex", 2, 0, NULL);
    return f;
}

void stext_free(stext_font *f)
{
    if (!f) return;
    for (int i = 0; i < f->nface; i++) {
        if (f->kind == K_PVF) scePvfClose(f->face[i].handle);
        else sceFontClose(f->face[i].handle);
    }
    if (f->lib) {
        if (f->kind == K_PVF) scePvfDoneLib(f->lib);
        else sceFontDoneLib(f->lib);
    }
    for (int i = 0; i < f->npage; i++) { vita2d_free_texture(f->page[i]->tex); free(f->page[i]); }
    for (int i = 0; i < f->nretired; i++) vita2d_free_texture(f->retired[i]);
    free(f->tab); free(f->scratch); free(f->fscratch);
    sceKernelDeleteLwMutex(&f->mutex);
    free(f);
}

static int pvf_lib(stext_font *f, int nfonts)
{
    ScePvfInitRec init = { f, (ScePvfU32)nfonts, NULL, NULL, lib_alloc, lib_realloc, lib_free };
    ScePvfError err = 0;
    f->lib = scePvfNewLib(&init, &err);
    if (err != 0) { f->lib = NULL; return -1; }
    /* the same library settings as vita2d, so scale 1.0 is the same size */
    scePvfSetEM(f->lib, 72.0f / (PVF_BASE * 128.0f));
    scePvfSetResolution(f->lib, 128.0f, 128.0f);
    f->vsize = PVF_BASE;
    return 0;
}

stext_font *stext_load_system_pvf(int n, const vita2d_system_pvf_config *cfg)
{
    if (n < 1 || n > MAX_FONTS) return NULL;
    stext_font *f = font_new(K_PVF);
    if (!f) return NULL;
    if (pvf_lib(f, n) < 0) { stext_free(f); return NULL; }
    for (int i = 0; i < n; i++) {
        ScePvfFontStyleInfo st;
        memset(&st, 0, sizeof st);
        st.languageCode = cfg[i].code;
        st.familyCode = SCE_PVF_DEFAULT_FAMILY_CODE;
        st.style = SCE_PVF_DEFAULT_STYLE_CODE;
        ScePvfError err = 0;
        ScePvfFontIndex idx = scePvfFindOptimumFont(f->lib, &st, &err);
        if (err != 0) { stext_free(f); return NULL; }
        ScePvfFontId h = scePvfOpen(f->lib, idx, 0, &err);
        if (err != 0) { stext_free(f); return NULL; }
        f->face[f->nface].handle = h;
        f->face[f->nface].in_group = cfg[i].in_font_group;
        f->face[f->nface].cur_size = -1;
        f->nface++;
    }
    return f;
}

stext_font *stext_load_default_pvf(void)
{
    vita2d_system_pvf_config c[] = { { SCE_PVF_DEFAULT_LANGUAGE_CODE, NULL } };
    return stext_load_system_pvf(1, c);
}

stext_font *stext_load_custom_pvf(const char *path)
{
    stext_font *f = font_new(K_PVF);
    if (!f) return NULL;
    if (pvf_lib(f, 1) < 0) { stext_free(f); return NULL; }
    ScePvfError err = 0;
    ScePvfFontId h = scePvfOpenUserFile(f->lib, (ScePvfPointer)path, 1, &err);
    if (err != 0) { stext_free(f); return NULL; }
    f->face[0].handle = h; f->face[0].cur_size = -1; f->nface = 1;
    return f;
}

static int pgf_lib(stext_font *f, int nfonts)
{
    SceFontNewLibParams p = { f, (unsigned int)nfonts, NULL, lib_alloc, lib_free, NULL, NULL, NULL, NULL, NULL, NULL };
    unsigned int err = 0;
    f->lib = sceFontNewLib(&p, &err);
    if (err != 0) { f->lib = NULL; return -1; }
    return 0;
}

static void pgf_post(stext_font *f)
{
    SceFontInfo fi;
    memset(&fi, 0, sizeof fi);
    sceFontGetFontInfo(f->face[0].handle, &fi);
    f->vsize = fi.fontStyle.fontVRes > 0 ? fi.fontStyle.fontV / fi.fontStyle.fontVRes * SCREEN_DPI : 20.0f;
}

stext_font *stext_load_system_pgf(int n, const vita2d_system_pgf_config *cfg)
{
    if (n < 1 || n > MAX_FONTS) return NULL;
    stext_font *f = font_new(K_PGF);
    if (!f) return NULL;
    if (pgf_lib(f, n) < 0) { stext_free(f); return NULL; }
    for (int i = 0; i < n; i++) {
        SceFontStyle st;
        memset(&st, 0, sizeof st);
        st.fontH = 10; st.fontV = 10;
        st.fontLanguage = cfg[i].code;
        unsigned int err = 0;
        int idx = sceFontFindOptimumFont(f->lib, &st, &err);
        if (err != 0) { stext_free(f); return NULL; }
        SceFontHandle h = sceFontOpen(f->lib, idx, 0, &err);
        if (err != 0) { stext_free(f); return NULL; }
        f->face[f->nface].handle = h;
        f->face[f->nface].in_group = cfg[i].in_font_group;
        f->nface++;
    }
    pgf_post(f);
    return f;
}

stext_font *stext_load_default_pgf(void)
{
    vita2d_system_pgf_config c[] = { { SCE_FONT_LANGUAGE_DEFAULT, NULL } };
    return stext_load_system_pgf(1, c);
}

stext_font *stext_load_custom_pgf(const char *path)
{
    stext_font *f = font_new(K_PGF);
    if (!f) return NULL;
    if (pgf_lib(f, 1) < 0) { stext_free(f); return NULL; }
    unsigned int err = 0;
    SceFontHandle h = sceFontOpenUserFile(f->lib, (char *)path, 1, &err);
    if (err != 0) { stext_free(f); return NULL; }
    f->face[0].handle = h; f->nface = 1;
    pgf_post(f);
    return f;
}
