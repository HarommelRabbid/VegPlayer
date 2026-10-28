/* veg.c - Vegas Pro .veg project reader. See veg.h.
 *
 * File layout: Wave64-style chunks (16-byte GUID + 64-bit size, 8-byte aligned) nested in "riff"/"list"
 * chunks (a list carries a 16-byte type GUID after its header). The project looks like this:
 *
 *   list 4d6c0748  track
 *     4d6c0749       track header (u32 @0x0c: 3 = video, 2 = audio)
 *     list 4d6c0749  events
 *       list 4d6c074a  event
 *         168d206a       event (start, length, rate, kind, level, fades)
 *         168d206d       take (in-point, media id, name)
 *         884bacff       Pan/Crop keyframes ("POSK")
 *         list 1af0e4e4  -> 1af0e4e6 video effect (legacy DXT or OFX)
 *         list d54ad82a  -> d54ad82a audio time stretch / pitch
 *         list b66a95ce  -> b66a95ce audio DirectX plug-in chain
 *     list 69d46c28 -> 69d46c29  video track compositing (Level @0x68)
 *     list 69d46c26 -> 69d46c27  audio track settings (volume @0x0c)
 *     list d24df987 -> cd6915d8  track motion
 *   list b28f2d5b -> b28f2d5d   media pool entries (id @0x3c, UTF-16 path @0x60)
 *   b28f2d5a                    project settings (sample rate @0x0c, fps @0x10, frame size)
 */
#include "veg.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static double rdd(const uint8_t *p) { uint64_t v = rd64(p); double d; memcpy(&d, &v, 8); return d; }
static float rdf(const uint8_t *p) { uint32_t v = rd32(p); float f; memcpy(&f, &v, 4); return f; }

/* GUID as text, the way Python's uuid.UUID(bytes_le=...) prints it */
static void guid_str(const uint8_t *b, char out[37])
{
    static const char hx[] = "0123456789abcdef";
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hx[b[order[i]] >> 4];
        out[o++] = hx[b[order[i]] & 15];
    }
    out[o] = 0;
}

static const uint8_t *mem_find(const uint8_t *h, size_t hn, const void *needle, size_t nn)
{
    const uint8_t *n = needle;
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return NULL;
}

static void put_utf8(char *out, size_t outsz, size_t *o, uint32_t c)
{
    char b[4]; int k;
    if (c < 0x80) { b[0] = (char)c; k = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); k = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
    if (*o + (size_t)k + 1 > outsz) return;
    memcpy(out + *o, b, (size_t)k);
    *o += (size_t)k;
}

/* UTF-16LE (until NUL or nbytes) -> UTF-8 */
static void u16_utf8(const uint8_t *p, size_t nbytes, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < nbytes; i += 2) {
        uint32_t c = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < nbytes) {
            uint32_t d = (uint32_t)p[i + 2] | (uint32_t)p[i + 3] << 8;
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; }
            else continue;
        } else if (c >= 0xD800 && c < 0xE000) continue;
        put_utf8(out, outsz, &o, c);
    }
    out[o] = 0;
}

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static int ci_contains(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) {
        size_t k = 0;
        while (k < nl && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)n[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static const char *ci_find(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) {
        size_t k = 0;
        while (k < nl && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)n[k])) k++;
        if (k == nl) return h;
    }
    return NULL;
}

const char *veg_basename(const char *path)
{
    const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

const char *veg_ext(const char *path)
{
    const char *b = veg_basename(path), *d = strrchr(b, '.');
    return (d && d != b) ? d : "";
}

#define GROW(arr, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 16; \
    void *np_ = realloc((arr), sizeof(*(arr)) * (size_t)(cap)); if (!np_) return -1; (arr) = np_; } } while (0)

/* ------------------------------------------------------------------ GUIDs */

#define G_EVENT_LIST "4d6c074a-2316-11d2-86b0-00c04f8edb8a"
#define G_EVENT      "168d206a-2321-11d2-86b0-00c04f8edb8a"
#define G_TAKE       "168d206d-2321-11d2-86b0-00c04f8edb8a"
#define G_TRACK_HDR  "4d6c0749-2316-11d2-86b0-00c04f8edb8a"
#define G_MEDIA      "b28f2d5d-230f-11d2-86af-00c04f8edb8a"
#define G_PROJECT    "b28f2d5a"
#define G_PANCROP    "884bacff"
#define G_VFX        "1af0e4e6"
#define G_STRETCH    "d54ad82a"
#define G_AFX_CHAIN  "b66a95ce"
#define G_COMP       "69d46c29"
#define G_ACOMP      "69d46c27"   /* audio track settings: volume (float, linear) @0x0c */
#define G_TRACKMOTION "cd6915d8"
static void track_motion_fx(const uint8_t *b, size_t bl, VegTrack *t);
#define G_TRACKFX    "f79298f4"   /* audio track FX chain (Track Noise Gate/EQ/Compressor + inserted plug-ins) */

typedef struct { char name[64]; double v[4]; int nv; } OfxParam;
static int ofx_params(const uint8_t *b, size_t bl, char *id, size_t idsz, OfxParam *out, int maxp);
static const OfxParam *ofx_find(const OfxParam *ps, int n, const char *name);
/* a2fc1a41-01f6-460e-ae5a-2be4af57b92e as stored */
static const uint8_t OFX_WRAP[16] = { 0x41,0x1a,0xfc,0xa2,0xf6,0x01,0x0e,0x46,0xae,0x5a,0x2b,0xe4,0xaf,0x57,0xb9,0x2e };
static const uint8_t SOLID_GEN[16] = { 0xfc,0x0d,0xdd,0xd9,0x4f,0x27,0xa7,0x45,0xb8,0x74,0x3d,0x2b,0xcf,0xa6,0x1f,0xe9 };
static const uint8_t TEXT_GEN[16]  = { 0x9d,0x78,0xe8,0x0f,0x47,0x0c,0x2a,0x44,0xaf,0xb0,0x0d,0xaf,0x97,0x66,0x93,0x17 };
static const uint8_t NOISE_NAME[6] = { 0xce,0x30,0xa4,0x30,0xba,0x30 };            /* "ノイズ" UTF-16LE */
static const uint8_t MEDIA_ID_MARK[8] = { 0xff,0xff,0xff,0xff,0,0,0,0 };
static const uint8_t PRESET_MARK[12] = { 0x0b,0,0,0, 0x06,0x01,0,0, 0x01,0,0,0 };

#define FX_BW_GUID     "e87674d5-e8f2-11d1-9aa9-00a0c99b12c5"   /* Sony Black and White (DXT) */
#define FX_INVERT_GUID "c3062be7-e69d-11d1-9aa9-00a0c99b12c5"   /* Sony Invert (DXT) */
#define FX_OFX_GUID    "a2fc1a41-01f6-460e-ae5a-2be4af57b92e"   /* OFX host wrapper: the effect is named by an Svfx id */
#define FX_CHANBLEND_GUID "0bc15b73-83b9-11d4-9f67-00485481bd31" /* Sony Channel Blend (DXT) */
#define FX_GRADMAP_GUID   "300f2f5f-d7c7-48be-b796-90fc740b654e" /* Sony Gradient Map (DXT) */
#define FX_LINBLUR_GUID   "8dbe8301-d9a5-4fd5-8901-1d8bf4d3a6c0" /* Sony Linear Blur (DXT, Vegas 8) */
#define AFX_PITCH_GUID "ed1b4100-93be-11d0-aebc-00a0c9053912"   /* Sony Pitch Shift (DirectX) */
#define AFX_REVERB_GUID "607682e0-6e21-11d0-aebc-00a0c9053912"  /* Sony Reverb (DirectX) */

/* ------------------------------------------------------------------ chunk iteration */

typedef struct { size_t off, sz; int islist; char g[37]; size_t body, blen; } Chunk;

/* next chunk in [*pos, end). For lists g is the list *type* and body points at the children. */
static int next_chunk(const uint8_t *d, size_t *pos, size_t end, Chunk *c)
{
    size_t off = *pos;
    if (off + 24 > end) return 0;
    uint64_t sz = rd64(d + off + 16);
    if (sz < 24 || sz > end - off) return 0;
    c->off = off;
    c->sz = (size_t)sz;
    c->islist = !memcmp(d + off, "riff", 4) || !memcmp(d + off, "list", 4);
    if (c->islist) {
        if (sz < 40) return 0;
        guid_str(d + off + 24, c->g);
        c->body = off + 40; c->blen = (size_t)sz - 40;
    } else {
        guid_str(d + off, c->g);
        c->body = off + 24; c->blen = (size_t)sz - 24;
    }
    *pos = off + (((size_t)sz + 7) & ~(size_t)7);
    return 1;
}

static int gpre(const Chunk *c, const char *prefix) { return !strncmp(c->g, prefix, strlen(prefix)); }

/* ------------------------------------------------------------------ parse state */

typedef struct {
    const uint8_t *d; size_t n;
    VegProject *p;
    int evcap, mcap;
    int cur;            /* event receiving sub-chunks, -1 if none */
    int track;          /* current track */
    char unk[1024];
} St;

static void note_unknown(St *s, const char *what)
{
    if (strstr(s->unk, what)) return;
    size_t l = strlen(s->unk);
    if (l + strlen(what) + 3 < sizeof s->unk) {
        if (l) strcat(s->unk, ", ");
        strcat(s->unk, what);
    }
}

/* ------------------------------------------------------------------ project settings & media */

static void parse_project(St *s, const uint8_t *b, size_t bl)
{
    VegProject *p = s->p;
    if (bl >= 16) { uint32_t sr = rd32(b + 0x0c); if (sr >= 8000 && sr <= 192000) p->sample_rate = (int)sr; }
    if (bl >= 24) { double f = rdd(b + 16); if (f >= 1 && f <= 240) p->fps = f; }
    for (size_t k = 0; bl > 12 && k < bl - 12; k += 2) {
        uint32_t w = rd32(b + k), h = rd32(b + k + 4), tag = rd32(b + k + 8);
        if (tag == 0x1c && w >= 16 && w <= 16384 && h >= 16 && h <= 16384 && !(w & 1) && !(h & 1)) { p->width = (int)w; p->height = (int)h; break; }
    }
    /* UTF-16 strings in the chunk: the project's save path (ends in .veg / .vf) and the editor's settings folder */
    for (size_t k = 0; k + 16 <= bl; k += 2) {
        /* a run of UTF-16 characters (any alphabet): no NULs, no control characters */
        size_t n = 0;
        while (k + n + 1 < bl && (b[k + n] | b[k + n + 1]) && !(b[k + n + 1] == 0 && b[k + n] < 0x20)) n += 2;
        if (n < 12) continue;
        char str[512];
        u16_utf8(b + k, n, str, sizeof str);
        size_t L = strlen(str);
        if (!p->saved_path[0] && L > 4 && (ci_contains(str + L - 4, ".veg") || ci_contains(str + L - 3, ".vf")) && strchr(str, '\\'))
            snprintf(p->saved_path, sizeof p->saved_path, "%s", str);
        const char *ad = ci_find(str, "\\AppData\\Local\\");
        if (!ad) ad = ci_find(str, "\\Application Data\\");
        if (!p->app_dir[0] && ad) {
            snprintf(p->app_dir, sizeof p->app_dir, "%s", str);
            /* "...\Local\[Sony\]<product>\<version>\" */
            const char *q = strchr(ad + 1, '\\');
            q = q ? strchr(q + 1, '\\') : NULL;   /* after "AppData\" "Local\" */
            if (q) {
                char rest[256];
                snprintf(rest, sizeof rest, "%s", q + 1);
                char *parts[4] = { 0 }; int np = 0;
                for (char *t = strtok(rest, "\\"); t && np < 4; t = strtok(NULL, "\\")) parts[np++] = t;
                int sony = np && !strcmp(parts[0], "Sony");   /* Sony era: "Sony\\Vegas Pro\\11.0" */
                int pi = sony || (np && !strcmp(parts[0], "MAGIX")) ? 1 : 0;
                if (np > pi + 1)
                    snprintf(p->app_name, sizeof p->app_name, "%s %s%s", parts[pi], parts[pi + 1], sony ? " (Sony)" : "");
                else if (np > pi) snprintf(p->app_name, sizeof p->app_name, "%s", parts[pi]);
            }
        }
        k += n - 2;   /* (the loop adds 2) */
    }
}

static int parse_media(St *s, const uint8_t *b, size_t bl)
{
    VegProject *p = s->p;
    if (bl < 0x64) return 0;
    uint32_t id; size_t path_at;
    if (!memcmp(b + 0x30, MEDIA_ID_MARK, 8)) {
        id = rd32(b + 0x3c);
        /* the first word is the path's offset: 0x60 in most files, 0x80 in some, 0x50 in Vegas 8 */
        uint32_t po = rd32(b);
        path_at = po >= 0x40 && po < 0x200 && po < bl ? po : 0x60;
    }
    else {   /* older layout: search like vegtool did */
        const uint8_t *m = mem_find(b, bl, MEDIA_ID_MARK, 8);
        if (!m || (size_t)(m - b) + 16 > bl) return 0;
        id = rd32(m + 12); path_at = (size_t)(m - b) + 0x30;
        if (path_at >= bl) return 0;
    }
    int idx = -1;
    for (int i = 0; i < p->nmedia; i++) if (p->media[i].id == id) idx = i;   /* later entries win */
    if (idx < 0) {
        GROW(p->media, p->nmedia, s->mcap);
        idx = p->nmedia++;
    }
    VegMedia *m = &p->media[idx];
    memset(m, 0, sizeof *m);
    m->id = id;
    m->streams = (int)rd32(b + 0x38);
    u16_utf8(b + path_at, bl - path_at, m->path, sizeof m->path);
    snprintf(m->name, sizeof m->name, "%s", veg_basename(m->path));
    m->type = VEG_MEDIA_FILE;
    if (!strncmp(m->path, "META:", 5) || mem_find(b, bl, SOLID_GEN, 16) || mem_find(b, bl, TEXT_GEN, 16)) m->type = VEG_MEDIA_GENERATOR;
    if (mem_find(b, bl, SOLID_GEN, 16)) {
        /* Vegas 8-13 solid colour: four floats (B,G,R,A) after the preset name (vegtool) */
        const uint8_t *j = mem_find(b, bl, PRESET_MARK, 12);
        if (j && (size_t)(j - b) + 16 <= bl) {
            uint32_t n = rd32(j + 12);
            size_t jo = (size_t)(j - b);
            if (n < bl && jo + 32 + n <= bl) {
                m->type = VEG_MEDIA_SOLID;
                m->rgba[2] = rdf(j + 16 + n); m->rgba[1] = rdf(j + 20 + n);
                m->rgba[0] = rdf(j + 24 + n); m->rgba[3] = rdf(j + 28 + n);
                for (int k = 0; k < 4; k++) m->rgba[k] = m->rgba[k] < 0 ? 0 : m->rgba[k] > 1 ? 1 : m->rgba[k];
            }
        }
        if (m->type != VEG_MEDIA_SOLID) {
            /* Vegas 8 layout: the four floats (B, G, R, A) follow the generator GUID within ~0x60 bytes; take the
             * first run of four floats in 0..1 whose alpha is 1 */
            const uint8_t *g = mem_find(b, bl, SOLID_GEN, 16);
            for (size_t k = (size_t)(g - b) + 16; g && k + 16 <= bl && k < (size_t)(g - b) + 0x80; k += 2) {
                float c[4] = { rdf(b + k), rdf(b + k + 4), rdf(b + k + 8), rdf(b + k + 12) };
                int ok = c[3] == 1.0f;
                for (int q = 0; q < 3 && ok; q++) ok = c[q] >= 0 && c[q] <= 1 && (rd32(b + k + 4 * q) == 0 || rd32(b + k + 4 * q) > 0x30000000u);
                if (!ok) continue;
                m->type = VEG_MEDIA_SOLID;
                m->rgba[2] = c[0]; m->rgba[1] = c[1]; m->rgba[0] = c[2]; m->rgba[3] = 1;
                break;
            }
        }
    } else if (mem_find(b, bl, TEXT_GEN, 16) || ci_contains(m->path, "titlesandtext") || ci_contains(m->path, ":text}")) {
        m->type = VEG_MEDIA_TEXT;
    } else if (mem_find(b, bl, NOISE_NAME, 6) || ci_contains(m->path, "noisetexture")) {
        m->type = VEG_MEDIA_NOISE;
    } else if (ci_contains(m->path, "solidcolor")) {
        /* OFX solid colour (Vegas 14+, Movie Studio): an OFX block like the effects' (wrapper GUID, plug-in id,
         * preset name) with a "Color" parameter of four doubles, RGBA 0..1 */
        m->type = VEG_MEDIA_SOLID;
        m->rgba[0] = m->rgba[1] = m->rgba[2] = m->rgba[3] = 1;
        const uint8_t *g = mem_find(b, bl, OFX_WRAP, 16);
        if (g && g - b >= 4) {
            OfxParam ps[16];
            char id[128];
            const uint8_t *base = g - 4;   /* the effect layout's offsets count from 4 bytes before the GUID */
            int np = ofx_params(base, bl - (size_t)(base - b), id, sizeof id, ps, 16);
            const OfxParam *c = ofx_find(ps, np, "Color");
            if (c && c->nv >= 3) {
                for (int k = 0; k < 4; k++) {
                    double v = k < c->nv ? c->v[k] : 1.0;
                    m->rgba[k] = (float)(v < 0 ? 0 : v > 1 ? 1 : v);
                }
            }
        }
    }
    if (m->type == VEG_MEDIA_GENERATOR || m->type == VEG_MEDIA_TEXT || m->type == VEG_MEDIA_NOISE) {
        /* "META:\Video Generator\{Svfx:...} (3)": keep the generator id as the name */
        const char *br = strchr(m->path, '{');
        if (br) snprintf(m->name, sizeof m->name, "%s", br);
    }
    return 0;
}

static int walk_media(St *s, size_t off, size_t end)
{
    Chunk c; size_t pos = off;
    while (next_chunk(s->d, &pos, end, &c)) {
        if (!c.islist && !strcmp(c.g, G_MEDIA)) { if (parse_media(s, s->d + c.body, c.blen) < 0) return -1; }
        else if (!c.islist && gpre(&c, G_PROJECT) && !s->p->fps) parse_project(s, s->d + c.body, c.blen);
        if (c.islist && walk_media(s, c.body, c.body + c.blen) < 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ video effects */

static void vfx_defaults(VegVfx *v)
{
    memset(v, 0, sizeof *v);
    v->amount = 1; v->sat = 1; v->lum = 1; v->lut = -1;
}

/* Legacy (DXT) effect: header 0x38, preset name (UTF-16, byte length @0x34), then parameter data.
 * The first double after the name is the parameter; a keyframe block after it ("u32 8" + double) holds the
 * value actually used when present. */
static double dxt_param(const uint8_t *b, size_t bl, double dflt)
{
    if (bl < 0x38) return dflt;
    uint32_t n = rd32(b + 0x34);
    if (n >= 400 || 0x38 + n + 8 > bl) return dflt;
    size_t q = 0x38 + n;
    double v = rdd(b + q);
    for (size_t k = q + 8; k + 12 <= bl; k += 2) {
        if (rd32(b + k) == 8) {
            double kv = rdd(b + k + 4);
            if (kv >= 0 && kv <= 1.0000001 && (kv != 0 || v == 0)) return kv;
        }
    }
    return (v >= 0 && v <= 1.0000001) ? v : dflt;
}

/* OFX effect: header 0x30; @0x28 id byte length, @0x2c preset byte length; id + preset (UTF-16);
 * then u32 block size, u32 count and entries { u32 value size, u32 name bytes, name (UTF-16), value }.
 * Doubles come first in a value (a 12-byte value is double + u32). */
static int ofx_params(const uint8_t *b, size_t bl, char *id, size_t idsz, OfxParam *out, int maxp)
{
    if (bl < 0x30) return 0;
    uint32_t idl = rd32(b + 0x28), prl = rd32(b + 0x2c);
    if (idl > 512 || prl > 512 || 0x30 + idl + prl + 8 > bl) return 0;
    u16_utf8(b + 0x30, idl, id, idsz);
    size_t q = 0x30 + idl + prl;
    uint32_t count = rd32(b + q + 4);
    q += 8;
    int np = 0;
    for (uint32_t i = 0; i < count && q + 8 <= bl && np < maxp; i++) {
        uint32_t vs = rd32(b + q), nl = rd32(b + q + 4);
        if (nl > 256 || vs > 4096 || q + 8 + nl + vs > bl) break;
        OfxParam *pr = &out[np++];
        memset(pr, 0, sizeof *pr);
        u16_utf8(b + q + 8, nl, pr->name, sizeof pr->name);
        const uint8_t *v = b + q + 8 + nl;
        if (vs == 8) { pr->v[0] = (double)(int32_t)rd32(v); pr->nv = 1; }
        else for (uint32_t k = 0; k + 8 <= vs && pr->nv < 4; k += 8) pr->v[pr->nv++] = rdd(v + k);
        q += 8 + nl + vs;
    }
    return np;
}

static const OfxParam *ofx_find(const OfxParam *ps, int n, const char *name)
{
    for (int i = 0; i < n; i++) if (!strcmp(ps[i].name, name) || ci_contains(ps[i].name, name)) return &ps[i];
    return NULL;
}

static double ofx_get(const OfxParam *ps, int n, const char *name, double dflt)
{
    const OfxParam *p = ofx_find(ps, n, name);
    return p && p->nv ? p->v[0] : dflt;
}

/* DXT effects: after the preset name comes a static parameter block, then the keyframes. The first keyframe's copy
 * of the block holds the values Vegas actually uses (the static block can be stale): it starts with u32 28 placed
 * right after the static block, whose length is repeated 0x18 bytes later, followed by the copy itself. */
static const uint8_t *dxt_params(const uint8_t *b, size_t bl, size_t *len)
{
    if (bl < 0x38) return NULL;
    uint32_t n = rd32(b + 0x34);
    if (n >= 400 || 0x38 + n > bl) return NULL;
    size_t q = 0x38 + n;
    for (size_t k = q + 8; k + 0x1c <= bl; k += 4) {
        if (rd32(b + k) != 0x1c) continue;
        uint32_t S = rd32(b + k + 0x18);
        /* the copy has the static block's length (short static blocks, e.g. Linear Blur: 12 vs 16) */
        if ((S == k - q || (k - q < 32 && S >= 8 && S <= 64)) && k + 0x1c + S <= bl) { *len = S; return b + k + 0x1c; }
    }
    *len = bl - q;
    return b + q;
}

static void lab_or_blur_ofx(VegVfx *v, const char *id, const OfxParam *ps, int np)
{
    if (ci_contains(id, ":labadjust")) {
        v->type = VFX_LAB;
        v->lum = ofx_get(ps, np, "Lightness", 1);
        v->a = ofx_get(ps, np, "Channela", 0);
        v->b = ofx_get(ps, np, "Channelb", 0);
        v->invert = ofx_get(ps, np, "InvertLuminance", 0) != 0;
        v->desat = ofx_get(ps, np, "Desaturate", 0) != 0;
    } else if (ci_contains(id, ":linearblur")) {
        v->type = VFX_LINEARBLUR;
        v->angle = ofx_get(ps, np, "Angle", 0);
        v->amount = ofx_get(ps, np, "Amount", 0.05);
    } else if (ci_contains(id, ":radialblur")) {
        v->type = VFX_RADIALBLUR;
        v->amount = ofx_get(ps, np, "Amount", 0.1);
        const OfxParam *c = ofx_find(ps, np, "Center");
        v->cx = c && c->nv >= 2 ? c->v[0] : 0.5;
        v->cy = c && c->nv >= 2 ? 1.0 - c->v[1] : 0.5;   /* OFX y points up */
    } else if (ci_contains(id, ":gaussianblur")) {
        v->type = VFX_GAUSSBLUR;
        v->a = ofx_get(ps, np, "HorizontalRange", 0);
        v->b = ofx_get(ps, np, "VerticalRange", 0);
        if (v->a <= 0 && v->b <= 0) v->type = VFX_NOOP;
    }
}

static void parse_vfx(St *s, const uint8_t *b, size_t bl, VegVfx *v)
{
    vfx_defaults(v);
    if (bl < 0x20) return;
    char plug[37];
    guid_str(b + 4, plug);
    if (!strcmp(plug, FX_OFX_GUID)) {
        char id[200] = "";
        OfxParam ps[48];
        int np = ofx_params(b, bl, id, sizeof id, ps, 48);
        snprintf(v->name, sizeof v->name, "%s", id);
        if (ci_contains(id, ":hsladjust")) {
            v->type = VFX_HSL;
            v->hue = ofx_get(ps, np, "AddToHue", 0);
            v->sat = ofx_get(ps, np, "Saturation", 1);
            v->lum = ofx_get(ps, np, "Luminance", 1);
        } else if (ci_contains(id, ":invert")) {
            v->type = VFX_INVERT;
            v->amount = ofx_get(ps, np, "Amount", ofx_get(ps, np, "Blend", ofx_get(ps, np, "BlendAmount", 1)));
        } else if (ci_contains(id, ":channelblend")) {
            /* VEGAS 20 Channel Blend (OFX): "<OUT> weight R/G/B/A/Offset" for OUT = RED, GREEN, BLUE, ALPHA, and Normalize */
            static const char *OUT[3] = { "RED", "GREEN", "BLUE" }, *IN[4] = { "R", "G", "B", "Offset" };
            int norm = ofx_get(ps, np, "Normalize", 0) != 0;
            v->type = VFX_MATRIX;
            for (int o = 0; o < 3; o++) {
                double w[4], sum = 0;
                for (int k = 0; k < 4; k++) {
                    char nm[48];
                    snprintf(nm, sizeof nm, "%s weight %s", OUT[o], IN[k]);
                    w[k] = ofx_get(ps, np, nm, k == o ? 1 : 0);
                    if (!(w[k] > -16 && w[k] < 16)) w[k] = 0;
                    if (k < 3) sum += w[k];
                }
                double d = norm && fabs(sum) > 1e-9 ? sum : 1;
                for (int k = 0; k < 3; k++) v->mtx[o * 4 + k] = (float)(w[k] / d);
                v->mtx[o * 4 + 3] = (float)w[3];
            }
        } else if (ci_contains(id, ":blackandwhite") || ci_contains(id, ":monochrome") || ci_contains(id, ":grayscale")) {
            v->type = VFX_GRAYSCALE;
            v->amount = ofx_get(ps, np, "Amount", ofx_get(ps, np, "Blend", 1));
        } else if (ci_contains(id, ":sepia")) {
            v->type = VFX_SEPIA;
            v->amount = ofx_get(ps, np, "Amount", ofx_get(ps, np, "Blend", 1));
        } else if (ci_contains(id, ":brightnessandcontrast") || ci_contains(id, ":brightnesscontrast")) {
            v->type = VFX_BRICON;
            v->bright = ofx_get(ps, np, "Brightness", 0);
            v->contrast = ofx_get(ps, np, "Contrast", 0);
        } else if (ci_contains(id, ":labadjust") || ci_contains(id, ":linearblur") || ci_contains(id, ":radialblur") || ci_contains(id, ":gaussianblur")) {
            lab_or_blur_ofx(v, id, ps, np);
        } else if (ci_contains(id, ":colorcorrector") && !ci_contains(id, "secondary")) {
            /* saturation around luma, then gain and offset; the colour wheels and gamma are not applied */
            double sat = ofx_get(ps, np, "Saturation", 1), gain = ofx_get(ps, np, "Gain", 1), off = ofx_get(ps, np, "Offset", 0);
            v->type = fabs(sat - 1) < 1e-6 && fabs(gain - 1) < 1e-6 && fabs(off) < 1e-6 ? VFX_NOOP : VFX_COLORCORR;
            const double L[3] = { 0.299, 0.587, 0.114 };
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) v->mtx[i * 4 + j] = (float)(gain * ((1 - sat) * L[j] + (i == j ? sat : 0)));
                v->mtx[i * 4 + 3] = (float)off;
            }
        } else if (ci_contains(id, ":colorbalance")) {
            double r = ofx_get(ps, np, "Red", 0), g = ofx_get(ps, np, "Green", 0), bb = ofx_get(ps, np, "Blue", 0);
            v->type = fabs(r) + fabs(g) + fabs(bb) < 1e-6 ? VFX_NOOP : VFX_COLORCORR;
            for (int i = 0; i < 12; i++) v->mtx[i] = i % 5 == 0 ? 1.0f : 0.0f;
            v->mtx[3] = (float)(r * 0.5); v->mtx[7] = (float)(g * 0.5); v->mtx[11] = (float)(bb * 0.5);   /* scale guessed */
        } else {
            v->type = VFX_UNKNOWN;
        }
    } else {
        uint32_t n = bl >= 0x38 ? rd32(b + 0x34) : 0;
        char preset[128] = "";
        if (n > 0 && n < 400 && 0x38 + n <= bl) u16_utf8(b + 0x38, n, preset, sizeof preset);
        if (!strcmp(plug, FX_INVERT_GUID)) {
            v->type = VFX_INVERT;
            v->amount = dxt_param(b, bl, 1.0);
            if (v->amount <= 0) v->amount = 1.0;   /* the default preset stores 0 here; Invert is all-or-nothing in practice */
        } else if (!strcmp(plug, FX_BW_GUID)) {
            v->type = VFX_GRAYSCALE;
            v->amount = dxt_param(b, bl, 1.0);
        } else if (!strcmp(plug, FX_CHANBLEND_GUID)) {
            /* 2 x 20 doubles; the first block: for each output channel (R,G,B,A) its weights from input R,G,B,A and
             * one more value. "Red Only" = red output from R+G+B (normalised), green and blue zero: the picture's
             * brightness in red. The second block stays identity (not applied). */
            size_t pl;
            const uint8_t *pp = dxt_params(b, bl, &pl);
            if (pp && pl >= 20 * 8) {
                v->type = VFX_MATRIX;
                for (int o = 0; o < 3; o++) {
                    double row[3], sum = 0;
                    for (int in = 0; in < 3; in++) {
                        double w = rdd(pp + (size_t)(o * 5 + in) * 8);
                        row[in] = w > -16 && w < 16 ? w : 0;
                        sum += row[in];
                    }
                    double norm = sum > 1 ? sum : 1;   /* weights adding up past 1 are scaled back */
                    for (int in = 0; in < 3; in++) v->mtx[o * 4 + in] = (float)(row[in] / norm);
                    v->mtx[o * 4 + 3] = 0;
                }
            } else v->type = VFX_UNKNOWN;
        } else if (!strcmp(plug, FX_LINBLUR_GUID)) {
            /* doubles: angle (degrees), amount (as the OFX version) */
            size_t pl;
            const uint8_t *pp = dxt_params(b, bl, &pl);
            v->type = VFX_LINEARBLUR;
            v->angle = 0; v->amount = 0.05;
            if (pp && pl >= 16) {
                double a = rdd(pp), am = rdd(pp + 8);
                if (a > -1000 && a < 1000) v->angle = a;
                if (am >= 0 && am <= 1) v->amount = am;
            }
        } else if (!strcmp(plug, FX_GRADMAP_GUID)) {
            /* double amount, u32 -, u32 stop count, then 32-byte stops: floats B,G,R,A, double position, double - */
            size_t pl;
            const uint8_t *pp = dxt_params(b, bl, &pl);
            uint32_t ns = pp && pl >= 0x10 ? rd32(pp + 0x0c) : 0;
            if (ns >= 1 && ns <= VFX_MAX_STOPS && 0x10 + (size_t)ns * 0x20 <= pl) {
                v->type = VFX_GRADIENT;
                double amt = rdd(pp);
                v->amount = amt >= 0 && amt <= 1 ? amt : 1.0;
                v->nstops = (int)ns;
                for (uint32_t i = 0; i < ns; i++) {
                    const uint8_t *st = pp + 0x10 + i * 0x20;
                    double pos = rdd(st + 16);
                    v->stop_pos[i] = (float)(pos >= 0 && pos <= 1 ? pos : (double)i / (ns > 1 ? ns - 1 : 1));
                    for (int c = 0; c < 3; c++) {
                        float x = rdf(st + 8 - c * 4);   /* stored B, G, R */
                        v->stop_rgb[i][c] = x < 0 ? 0 : x > 1 ? 1 : x;
                    }
                }
                for (int i = 1; i < v->nstops; i++)   /* keep stops sorted by position */
                    for (int j = i; j > 0 && v->stop_pos[j] < v->stop_pos[j - 1]; j--) {
                        float tp = v->stop_pos[j]; v->stop_pos[j] = v->stop_pos[j - 1]; v->stop_pos[j - 1] = tp;
                        for (int c = 0; c < 3; c++) { float t = v->stop_rgb[j][c]; v->stop_rgb[j][c] = v->stop_rgb[j - 1][c]; v->stop_rgb[j - 1][c] = t; }
                    }
            } else v->type = VFX_UNKNOWN;
        } else v->type = VFX_UNKNOWN;
        snprintf(v->name, sizeof v->name, "%s%s%.8s", preset, *preset ? " " : "", plug);
    }
    if (v->amount < 0) v->amount = 0;
    if (v->amount > 1) v->amount = 1;
    if (v->type == VFX_NOOP) return;
    if (v->type == VFX_UNKNOWN) { s->p->n_unknown_vfx++; note_unknown(s, v->name); }
}

/* ------------------------------------------------------------------ audio plug-in chain */

/* A plug-in entry inside the chain chunk looks like
 *   [CLSID 16][u32 0][u32 name bytes][UTF-16 name][CLSID 16 again][u32 n][u32 n][n bytes: format GUID 16 + params]
 * The repeated CLSID makes it easy to find without knowing the header layout. */
static int parse_afx_chain(St *s, const uint8_t *b, size_t bl, VegAfx **arr, int *narr)
{
    int cap = *narr;
    for (size_t i = 0; i + 24 <= bl; i++) {
        if (rd32(b + i + 16) != 0) continue;
        uint32_t nl = rd32(b + i + 20);
        if (nl < 4 || nl > 512 || (nl & 1)) continue;
        size_t j = i + 24 + nl;
        if (j + 24 > bl || memcmp(b + i, b + j, 16)) continue;
        if (!memcmp(b + i, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)) continue;
        uint32_t n1 = rd32(b + j + 16), n2 = rd32(b + j + 20);
        if (n1 != n2 || n1 < 16 || j + 24 + n1 > bl) continue;
        const uint8_t *data = b + j + 24 + 16;      /* skip the parameter-format GUID */
        size_t dl = n1 - 16;

        VegAfx a;
        memset(&a, 0, sizeof a);
        char clsid[37];
        guid_str(b + i, clsid);
        u16_utf8(b + i + 24, nl, a.name, sizeof a.name);
        /* "ピッチ シフト" = Pitch Shift, "ビブラート" = Vibrato (Japanese UI) */
        int is_pitch = !strcmp(clsid, AFX_PITCH_GUID) || ci_contains(a.name, "pitch shift") || strstr(a.name, "\xE3\x83\x94\xE3\x83\x83\xE3\x83\x81");
        int is_vib = ci_contains(a.name, "vibrato") || strstr(a.name, "\xE3\x83\x93\xE3\x83\x96\xE3\x83\xA9\xE3\x83\xBC\xE3\x83\x88");
        /* "リバーブ" = Reverb */
        int is_rev = !strcmp(clsid, AFX_REVERB_GUID) || ci_contains(a.name, "reverb") || strstr(a.name, "\xE3\x83\xAA\xE3\x83\x90\xE3\x83\xBC\xE3\x83\x96");
        /* every audio track carries these in their neutral state; Smooth/Enhance is subtle enough to skip */
        /* (by CLSID: the names are localised, e.g. "トラック EQ", "EQ piste") */
        int is_noop = !strcmp(clsid, "869419dd-501f-11d3-8cdc-00c04f6b8e4c") ||   /* Track Noise Gate */
                      !strcmp(clsid, "8cb69a0a-10e8-11d2-9b89-00104b8d13c2") ||   /* Track EQ */
                      !strcmp(clsid, "23c9f225-40ec-11d2-9d36-00c04f8edc1e") ||   /* Track Compressor */
                      !strcmp(clsid, "9e3e4540-8339-11d0-aebc-00a0c9053912");     /* Smooth/Enhance */
        if (is_pitch && dl >= 0x20) {
            /* params: i32 semitones, i32 cents, i32 accuracy, i32 preserve duration, i32 classic mode, i32 -, double ratio */
            int32_t semis = (int32_t)rd32(data), cents = (int32_t)rd32(data + 4);
            double ratio = rdd(data + 0x18);
            double from_int = semis + cents / 100.0;
            a.type = AFX_PITCH_SHIFT;
            if (ratio > 0.01 && ratio < 100) {
                double from_ratio = 12.0 * log2(ratio);
                a.semitones = fabs(from_ratio - from_int) < 0.05 ? from_int : from_ratio;
            } else a.semitones = from_int;
            a.preserve = rd32(data + 0x0c) != 0;
            a.mode = (int)rd32(data + 0x10);
        } else if (is_vib) {
            /* Sony Vibrato (d6802ba0-…), i32 params: 1200, 1000, -, 3, -, -, rate (centi-Hz), depth (cents, signed),
             * 1000, second depth. Rate: the value that varies between instances (9.99 / 3.02 / 4.63 Hz); depth: the
             * -100 values read as cents. Both guessed from two projects; the rest is unknown. */
            a.type = AFX_VIBRATO;
            a.rate_hz = 5.0; a.depth = 1.0;
            if (dl >= 0x28) {
                int32_t rate = (int32_t)rd32(data + 0x1c), dep = (int32_t)rd32(data + 0x20);
                if (rate >= 10 && rate <= 5000) a.rate_hz = rate / 100.0;
                if (dep != 0 && dep >= -2400 && dep <= 2400) a.depth = (dep < 0 ? -dep : dep) / 100.0;
            }
        } else if (is_rev && dl >= 44) {
            /* Sony Reverb, i32 params (guessed from scan.veg): dry out, reverb out, early out (centi-dB), -, mode,
             * decay (0.1 s), pre-delay (ms), -, low cut (Hz), high cut on, high cut (Hz) */
            a.type = AFX_REVERB;
            a.dry_db = (int32_t)rd32(data) / 100.0;
            a.wet_db = (int32_t)rd32(data + 4) / 100.0;
            a.early_db = (int32_t)rd32(data + 8) / 100.0;
            a.decay_s = (int32_t)rd32(data + 20) / 10.0;
            a.predelay_ms = (int32_t)rd32(data + 24);
            a.hicut_hz = rd32(data + 36) ? (int32_t)rd32(data + 40) : 0;
            if (!(a.decay_s > 0.05 && a.decay_s < 30)) a.decay_s = 2.0;
            if (!(a.predelay_ms >= 0 && a.predelay_ms <= 500)) a.predelay_ms = 20;
            if (a.dry_db < -100 || a.dry_db > 12) a.dry_db = 0;
            if (a.wet_db < -100 || a.wet_db > 12) a.wet_db = -6;
            if (a.early_db < -100 || a.early_db > 12) a.early_db = -6;
        } else if (is_rev) {
            a.type = AFX_REVERB;
            a.dry_db = 0; a.wet_db = -6; a.early_db = -6; a.decay_s = 2.0; a.predelay_ms = 20; a.hicut_hz = 12000;
        } else if (is_noop) {
            a.type = AFX_NOOP;
        } else {
            a.type = AFX_UNKNOWN;
            s->p->n_unknown_afx++;
            note_unknown(s, a.name);
        }
        GROW(*arr, *narr, cap);
        (*arr)[(*narr)++] = a;
        i = j + 24 + n1 - 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ events */

static int new_event(St *s, const uint8_t *b, size_t bl)
{
    VegProject *p = s->p;
    GROW(p->events, p->nevents, s->evcap);
    VegEvent *e = &p->events[p->nevents];
    memset(e, 0, sizeof *e);
    e->seq = p->nevents;
    e->track = s->track;
    e->media = -1;
    e->kind = bl > 71 ? (b[71] == 0x20 ? VEG_KIND_AUDIO : b[71] == 0x30 ? VEG_KIND_VIDEO : VEG_KIND_OTHER) : VEG_KIND_OTHER;
    e->start = (double)rd64(b + 16) / VEG_TICKS;
    e->length = (double)rd64(b + 24) / VEG_TICKS;
    e->rate = rdd(b + 40);
    if (!(e->rate > 0.001 && e->rate < 1000)) e->rate = 1.0;
    e->level = 1.0f;
    /* two layouts: 184-byte chunks (newer Vegas / Movie Studio) and 160-byte ones (older Vegas) with the fields
     * after the kind byte 8 bytes earlier (no 8-byte block at 0x60). Offsets below are for the 184-byte layout. */
    int sh = bl >= 0xb8 ? 0 : 8;
    if (bl >= (size_t)(0x70 - sh)) {
        float lv = rdf(b + 0x6c - sh);
        if (lv >= 0 && lv <= 16) e->level = lv;
    }
    e->fade_in_curve = e->fade_out_curve = e->kind == VEG_KIND_VIDEO ? VEG_CURVE_SMOOTH : VEG_CURVE_LINEAR;
    if (bl >= (size_t)(0x7c - sh)) {
        int ci = (int32_t)rd32(b + 0x74 - sh), co = (int32_t)rd32(b + 0x78 - sh);
        if (ci == 1 || ci == 2 || ci == -2 || ci == 4 || ci == -4) e->fade_in_curve = ci;
        if (co == 1 || co == 2 || co == -2 || co == 4 || co == -4) e->fade_out_curve = co;
    }
    if (bl >= (size_t)(0x98 - sh) && (e->kind == VEG_KIND_AUDIO || e->kind == VEG_KIND_VIDEO)) {
        e->fade_in = (double)rd64(b + 0x88 - sh) / VEG_TICKS;
        e->fade_out = (double)rd64(b + 0x90 - sh) / VEG_TICKS;
        if (e->fade_in > e->length) e->fade_in = e->length;
        if (e->fade_out > e->length) e->fade_out = e->length;
    }
    s->cur = p->nevents++;
    return 0;
}

static int event_chunk(St *s, const Chunk *c)
{
    VegProject *p = s->p;
    const uint8_t *b = s->d + c->body;
    size_t bl = c->blen;
    VegEvent *e = s->cur >= 0 ? &p->events[s->cur] : NULL;

    if (!strcmp(c->g, G_TAKE) && e && bl >= 42) {
        if (e->has_take) return 0;          /* first take = active take */
        e->has_take = 1;
        e->offset = (double)rd64(b + 16) / VEG_TICKS;
        e->media_id = rd32(b + 36);
        u16_utf8(b + 42, bl - 42, e->take_name, sizeof e->take_name);
        trim(e->take_name);
    } else if (gpre(c, G_PANCROP) && e) {
        VegPanKey k[256];
        int n = 0;
        for (size_t q = 0; q + 8 <= bl && n < 256;) {
            const uint8_t *hit = mem_find(b + q, bl - q, "POSK", 4);
            if (!hit) break;
            q = (size_t)(hit - b);
            uint32_t s2 = rd32(b + q + 4);
            if (q + 8 + 80 <= bl && s2 >= 80) {
                const uint8_t *pl = b + q + 8;
                k[n].t = (double)(int64_t)rd64(pl) / VEG_TICKS;
                k[n].w = rdd(pl + 20); k[n].h = rdd(pl + 28);
                k[n].cx = rdd(pl + 36); k[n].cy = rdd(pl + 44);
                k[n].rot = rdd(pl + 52);
                uint32_t cv = rd32(pl + 16);   /* interpolation to the next keyframe */
                k[n].curve = veg_key_type(cv);
                if (isfinite(k[n].w) && isfinite(k[n].h) && isfinite(k[n].cx) && isfinite(k[n].cy) && isfinite(k[n].rot)) n++;
            }
            q += 4;
        }
        if (n) {
            free(e->pan);
            e->pan = malloc(sizeof(VegPanKey) * (size_t)n);
            if (!e->pan) return -1;
            memcpy(e->pan, k, sizeof(VegPanKey) * (size_t)n);
            e->npan = n;
            if (bl >= 72) {
                uint32_t fw = rd32(b + 64), fh = rd32(b + 68);
                if (fw >= 16 && fw <= 16384 && fh >= 16 && fh <= 16384) { e->pan_w = (int)fw; e->pan_h = (int)fh; }
            }
        }
    } else if (gpre(c, G_VFX) && e) {
        VegVfx v;
        parse_vfx(s, b, bl, &v);
        void *np = realloc(e->vfx, sizeof(VegVfx) * (size_t)(e->nvfx + 1));
        if (!np) return -1;
        e->vfx = np;
        e->vfx[e->nvfx++] = v;
    } else if (gpre(c, G_STRETCH) && e && bl >= 0x18) {
        /* method @0x0c; Vegas 9+: attribute @0x10, semitones (double) @0x14; Vegas 8 (24-byte chunk): semitones @0x10 */
        int v8 = bl < 0x1c;
        e->has_stretch = 1;
        e->stretch_method = (int)rd32(b + 0x0c);
        e->stretch_attr = v8 ? 0 : (int)rd32(b + 0x10);
        double ps = rdd(b + (v8 ? 0x10 : 0x14));
        e->pitch_semis = (ps > -100 && ps < 100) ? ps : 0;
    } else if (gpre(c, G_AFX_CHAIN) && e) {
        if (parse_afx_chain(s, b, bl, &e->afx, &e->nafx) < 0) return -1;
    } else return 1;   /* not an event sub-chunk */
    return 0;
}

static int walk_track(St *s, size_t off, size_t end, int in_event)
{
    VegProject *p = s->p;
    VegTrack *t = &p->tracks[s->track];
    Chunk c; size_t pos = off;
    while (next_chunk(s->d, &pos, end, &c)) {
        if (c.islist) {
            if (!strcmp(c.g, G_EVENT_LIST)) {
                int saved = s->cur;
                s->cur = -1;
                if (walk_track(s, c.body, c.body + c.blen, 1) < 0) return -1;
                s->cur = in_event ? saved : -1;
            } else if (walk_track(s, c.body, c.body + c.blen, in_event) < 0) return -1;
            continue;
        }
        const uint8_t *b = s->d + c.body;
        size_t bl = c.blen;
        if (!strcmp(c.g, G_EVENT) && bl >= 48) {
            if (new_event(s, b, bl) < 0) return -1;
        } else if (!strcmp(c.g, G_TRACK_HDR) && bl >= 16 && !in_event) {
            uint32_t ty = rd32(b + 0x0c);
            t->kind = ty == 3 ? VEG_KIND_VIDEO : ty == 2 ? VEG_KIND_AUDIO : VEG_KIND_OTHER;
        } else if (gpre(&c, G_COMP) && bl >= 0x84 && !in_event) {
            t->has_comp = 1; t->comp_a = rd32(b + 0x60); t->comp_b = (int32_t)rd32(b + 0x64);
            if (t->comp_b >= VEG_COMP_CUSTOM && t->comp_b <= VEG_COMP_DIFFSQUARED) t->comp_mode = t->comp_b;
            float lv = rdf(b + 0x68);   /* track Level (opacity) */
            if (lv >= 0 && lv <= 1) t->level = lv;
        } else if (gpre(&c, G_ACOMP) && bl >= 0x10 && !in_event) {
            float vol = rdf(b + 0x0c);   /* linear gain: Vegas's -inf..+12 dB slider = 0..3.98 */
            if (vol >= 0 && vol <= 4.5f) t->level = vol;
        } else if (gpre(&c, G_TRACKMOTION) && bl >= 136 && !in_event) {
            t->has_motion = 1;
            t->tm_x = rdd(b + 88); t->tm_y = rdd(b + 96); t->tm_w = rdd(b + 120); t->tm_h = rdd(b + 128);
            track_motion_fx(b, bl, t);
        } else if (!in_event && s->cur < 0 && gpre(&c, G_VFX)) {
            VegVfx v;   /* track-level video FX */
            parse_vfx(s, b, bl, &v);
            void *np = realloc(t->vfx, sizeof(VegVfx) * (size_t)(t->nvfx + 1));
            if (!np) return -1;
            t->vfx = np;
            t->vfx[t->nvfx++] = v;
        } else if (!in_event && s->cur < 0 && (gpre(&c, G_AFX_CHAIN) || gpre(&c, G_TRACKFX))) {
            if (parse_afx_chain(s, b, bl, &t->afx, &t->nafx) < 0) return -1;
        } else {
            int r = event_chunk(s, &c);   /* also covers old files whose events aren't wrapped in event lists */
            if (r < 0) return -1;
        }
    }
    return 0;
}

/* Track Motion chunk (cd6915d8): u32 @0x04 = enabled parts (1 position, 8 2D Shadow, 16 2D Glow), then three
 * keyframe lists from @0x24 (position, shadow, glow), each: u32 count, then per keyframe a u32 size (counting
 * itself) and the data. In a keyframe at s (its size field): x @s+0x30, y @+0x38, w @+0x50, h @+0x58 (doubles);
 * shadow/glow colour @+0xb0 (4 floats, B G R A) and blur @+0xc0 (double). Position keyframes also: interpolation
 * type @s+4 (VEG_KEY_*, as Pan/Crop), time @s+8 (u64, 100 ns, project timeline), rotation as a quaternion
 * (w @s+0x90, x, y, z @s+0xa8; only z turns a 2D track: angle = 2 atan2(z, w)). Shadow/glow: first keyframe only. */
static void track_motion_fx(const uint8_t *b, size_t bl, VegTrack *t)
{
    uint32_t flags = rd32(b + 4);
    size_t p = 0x24;
    for (int blk = 0; blk < 3; blk++) {
        if (p + 8 > bl) return;
        uint32_t n = rd32(b + p);
        p += 4;
        size_t s = p;   /* first keyframe */
        if (blk == 0 && n > 0 && n < 100000) {
            free(t->tmk);
            t->tmk = calloc(n, sizeof(VegTmKey));
            t->ntmk = 0;
        }
        for (uint32_t k = 0; k < n; k++) {
            uint32_t sz = rd32(b + p);
            if (sz < 8 || sz > bl - p) return;
            if (blk == 0 && t->tmk && sz >= 0xb0) {
                VegTmKey key;
                const uint8_t *q = b + p;
                key.curve = veg_key_type(rd32(q + 4));   /* same numbering as Pan/Crop */
                key.t = (double)rd64(q + 8) * 1e-7;
                key.x = rdd(q + 0x30); key.y = rdd(q + 0x38); key.w = rdd(q + 0x50); key.h = rdd(q + 0x58);
                double qw = rdd(q + 0x90), qz = rdd(q + 0xa8);
                key.rot = isfinite(qw) && isfinite(qz) && (qw != 0 || qz != 0) ? 2.0 * atan2(qz, qw) : 0;
                if (isfinite(key.t) && isfinite(key.x) && isfinite(key.y) && isfinite(key.w) && isfinite(key.h) &&
                    fabs(key.x) < 1000 && fabs(key.y) < 1000 && fabs(key.w) < 1000 && fabs(key.h) < 1000)
                    t->tmk[t->ntmk++] = key;
            }
            p += sz;
        }
        if (blk == 0 && t->tmk) {   /* time order (insertion sort: few keyframes) */
            for (int i = 1; i < t->ntmk; i++)
                for (int j = i; j > 0 && t->tmk[j].t < t->tmk[j - 1].t; j--) { VegTmKey x = t->tmk[j]; t->tmk[j] = t->tmk[j - 1]; t->tmk[j - 1] = x; }
            if (!t->ntmk) { free(t->tmk); t->tmk = NULL; }
        }
        if (blk == 0 || n == 0 || s + 0xc8 > bl || rd32(b + s) < 0xc8) continue;
        VegTmFx *f = blk == 1 ? &t->shadow : &t->glow;
        f->on = !!(flags & (blk == 1 ? 8u : 16u));
        f->x = rdd(b + s + 0x30); f->y = rdd(b + s + 0x38); f->w = rdd(b + s + 0x50); f->h = rdd(b + s + 0x58);
        f->rgba[2] = rdf(b + s + 0xb0); f->rgba[1] = rdf(b + s + 0xb4); f->rgba[0] = rdf(b + s + 0xb8); f->rgba[3] = rdf(b + s + 0xbc);
        f->blur = rdd(b + s + 0xc0);
        int ok = isfinite(f->x) && isfinite(f->y) && isfinite(f->w) && isfinite(f->h) && isfinite(f->blur) &&
                 fabs(f->x) < 100 && fabs(f->y) < 100 && fabs(f->w) < 100 && fabs(f->h) < 100 && f->blur >= 0 && f->blur < 100;
        for (int c = 0; c < 4; c++) { if (!isfinite(f->rgba[c])) ok = 0; else f->rgba[c] = f->rgba[c] < 0 ? 0 : f->rgba[c] > 1 ? 1 : f->rgba[c]; }
        if (!ok) memset(f, 0, sizeof *f);
    }
}

/* a track is a list whose first child is the track header chunk */
static int is_track_list(St *s, const Chunk *c)
{
    Chunk f; size_t p2 = c->body;
    return next_chunk(s->d, &p2, c->body + c->blen, &f) && !f.islist && !strcmp(f.g, G_TRACK_HDR);
}

static int find_tracks(St *s, size_t off, size_t end, int *tcap)
{
    VegProject *p = s->p;
    Chunk c; size_t pos = off;
    while (next_chunk(s->d, &pos, end, &c)) {
        if (!c.islist) continue;
        if (is_track_list(s, &c)) {
            GROW(p->tracks, p->ntracks, *tcap);
            VegTrack *t = &p->tracks[p->ntracks];
            memset(t, 0, sizeof *t);
            t->kind = VEG_KIND_OTHER;
            t->level = 1.0f;
            t->comp_mode = VEG_COMP_SRCALPHA;
            s->track = p->ntracks++;
            s->cur = -1;
            if (walk_track(s, c.body, c.body + c.blen, 0) < 0) return -1;
        } else if (find_tracks(s, c.body, c.body + c.blen, tcap) < 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ public */

const char *veg_comp_name(int mode)
{
    static const char *N[] = { "3D Source Alpha", "Add", "Subtract", "Cut", "Source Alpha", "Multiply", "Overlay", "Screen",
                               "Hard Light", "Dodge", "Burn", "Darken", "Lighten", "Difference", "Difference Squared" };
    if (mode == VEG_COMP_CUSTOM) return "Custom (plug-in)";
    return mode >= 0 && mode <= VEG_COMP_DIFFSQUARED ? N[mode] : "?";
}

int veg_key_type(uint32_t stored)
{
    static const int MAP[] = { VEG_KEY_LINEAR, VEG_KEY_HOLD, VEG_KEY_SLOW, VEG_KEY_FAST, VEG_KEY_SMOOTH, VEG_KEY_SHARP };
    return stored < 6 ? MAP[stored] : VEG_KEY_LINEAR;
}

float veg_fade_shape(int curve, double u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    switch (curve) {
    case VEG_CURVE_FAST:   return (float)(1 - (1 - u) * (1 - u));        /* rises quickly, then levels off */
    case VEG_CURVE_SLOW:   return (float)(u * u);                        /* starts slowly */
    case VEG_CURVE_SMOOTH: return (float)(u * u * (3 - 2 * u));          /* S-curve */
    case VEG_CURVE_SHARP:  return (float)(0.5 - sin(asin(1 - 2 * u) / 3));   /* inverse S-curve */
    default:               return (float)u;
    }
}

static int cmp_ev(const void *a, const void *b)
{
    const VegEvent *x = a, *y = b;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

int veg_parse(const uint8_t *data, size_t len, VegProject *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    if (len < 40 || (memcmp(data, "riff", 4) && memcmp(data, "RIFF", 4))) {
        snprintf(err, errlen, "not a Vegas project (no riff header)");
        return -1;
    }
    St s;
    memset(&s, 0, sizeof s);
    s.d = data; s.n = len; s.p = p; s.cur = -1;
    int tcap = 0;
    if (walk_media(&s, 0, len) < 0 || find_tracks(&s, 0, len, &tcap) < 0) {
        snprintf(err, errlen, "out of memory while reading the project");
        veg_free(p);
        return -1;
    }
    if (!p->ntracks) { snprintf(err, errlen, "no tracks found - unsupported Vegas version?"); veg_free(p); return -1; }
    if (!p->fps) p->fps = 29.97;
    if (!p->width) { p->width = 1280; p->height = 720; }
    if (!p->sample_rate) p->sample_rate = 48000;

    /* drop events without a take (empty events), resolve media, sort per track */
    int w = 0;
    for (int i = 0; i < p->nevents; i++) {
        VegEvent *e = &p->events[i];
        if (!e->has_take || e->length <= 0 || e->kind == VEG_KIND_OTHER) {
            free(e->pan); free(e->vfx); free(e->afx);
            continue;
        }
        for (int m = 0; m < p->nmedia; m++) if (p->media[m].id == e->media_id) { e->media = m; break; }
        if (e->media < 0) {   /* fall back to the take name (file name without extension) */
            for (int m = 0; m < p->nmedia; m++) {
                char stem[256];
                snprintf(stem, sizeof stem, "%s", p->media[m].name);
                char *dot = strrchr(stem, '.');
                if (dot) *dot = 0;
                if (!strcmp(stem, e->take_name) || !strcmp(p->media[m].name, e->take_name)) { e->media = m; break; }
            }
        }
        p->events[w++] = *e;
    }
    p->nevents = w;
    qsort(p->events, (size_t)p->nevents, sizeof(VegEvent), cmp_ev);
    for (int t = 0; t < p->ntracks; t++) { p->tracks[t].first_event = 0; p->tracks[t].nevents = 0; }
    for (int i = 0; i < p->nevents; i++) {
        VegEvent *e = &p->events[i];
        VegTrack *t = &p->tracks[e->track];
        if (!t->nevents) t->first_event = i;
        t->nevents++;
        if (t->kind == VEG_KIND_OTHER) t->kind = e->kind;
        if (e->start + e->length > p->duration) p->duration = e->start + e->length;
        if (e->media >= 0) {
            VegMedia *m = &p->media[e->media];
            int k = e->kind == VEG_KIND_VIDEO ? 0 : 1;
            if (k == 0) m->used_video++; else m->used_audio++;
            double a = e->offset * e->rate, bnd = (e->offset + e->length) * e->rate;
            if ((k == 0 ? m->used_video : m->used_audio) == 1 || a < m->use_lo[k]) m->use_lo[k] = a;
            if ((k == 0 ? m->used_video : m->used_audio) == 1 || bnd > m->use_hi[k]) m->use_hi[k] = bnd;
        }
    }
    /* overlapping events on one track crossfade in Vegas: the later one fades in over the overlap */
    for (int t = 0; t < p->ntracks; t++) {
        VegTrack *tr = &p->tracks[t];
        for (int i = tr->first_event; i + 1 < tr->first_event + tr->nevents; i++) {
            VegEvent *a = &p->events[i], *b = &p->events[i + 1];
            double ov = a->start + a->length - b->start;
            if (ov > 1e-6 && b->start > a->start) {
                if (ov > b->length) ov = b->length;
                if (ov > b->xfade_in) b->xfade_in = ov;
                if (ov > a->xfade_out) a->xfade_out = ov;
            }
        }
    }
    snprintf(p->unknown_fx, sizeof p->unknown_fx, "%s", s.unk);
    return 0;
}

int veg_load_file(const char *path, VegProject *p, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "can't open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); snprintf(err, errlen, "empty file"); return -1; }
    uint8_t *d = malloc((size_t)n);
    if (!d) { fclose(f); snprintf(err, errlen, "out of memory (%ld bytes)", n); return -1; }
    size_t got = fread(d, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(d); snprintf(err, errlen, "read error"); return -1; }
    int r = veg_parse(d, (size_t)n, p, err, errlen);
    free(d);
    return r;
}

void veg_free(VegProject *p)
{
    for (int i = 0; i < p->nevents; i++) { free(p->events[i].pan); free(p->events[i].vfx); free(p->events[i].afx); }
    for (int t = 0; t < p->ntracks; t++) { free(p->tracks[t].vfx); free(p->tracks[t].afx); free(p->tracks[t].tmk); }
    free(p->events); free(p->tracks); free(p->media);
    memset(p, 0, sizeof *p);
}

int veg_track_seek(const VegProject *p, const VegTrack *t, double when)
{
    /* events are sorted by start; overlapping events are rare, so search on start and step back a little */
    int lo = t->first_event, hi = t->first_event + t->nevents;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (p->events[mid].start <= when) lo = mid + 1; else hi = mid;
    }
    int i = lo;
    /* walk back over events that started earlier but still play at `when` (bounded) */
    int k = i - 1, steps = 0;
    while (k >= t->first_event && steps < 64) {
        if (p->events[k].start + p->events[k].length > when) i = k;
        k--; steps++;
    }
    return i;
}
