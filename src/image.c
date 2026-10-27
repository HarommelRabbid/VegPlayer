/* image.c - picture files (PNG, JPEG, BMP, GIF first frame, TGA, PSD, PNM, HDR...) via stb_image into the cache */
#include "common.h"
#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_FAILURE_USERMSG
#include "../third_party/stb_image.h"

/* ------------------------------------------------------------------ MP3 cover art (ID3v2 APIC / PIC frame) */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t syncsafe(const uint8_t *p) { return (uint32_t)(p[0] & 0x7f) << 21 | (uint32_t)(p[1] & 0x7f) << 14 | (uint32_t)(p[2] & 0x7f) << 7 | (p[3] & 0x7f); }

/* undo ID3 unsynchronisation (FF 00 -> FF) in place; returns the new length */
static size_t id3_unsync(uint8_t *p, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xff && i + 1 < n && p[i + 1] == 0) i++;
    }
    return o;
}

/* skip a string in ID3 text encoding enc (0 Latin-1, 3 UTF-8: one NUL; 1, 2 UTF-16: a 16-bit NUL) */
static size_t id3_skip_str(const uint8_t *p, size_t n, int enc)
{
    size_t i = 0;
    if (enc == 1 || enc == 2) { while (i + 1 < n && (p[i] || p[i + 1])) i += 2; return i + 2 <= n ? i + 2 : n; }
    while (i < n && p[i]) i++;
    return i + 1 <= n ? i + 1 : n;
}

/* the embedded picture of an MP3 (front cover preferred, else the first one); points into buf (which may be
 * rewritten in place to undo unsynchronisation). NULL if there's none. */
static const uint8_t *id3_cover(uint8_t *buf, size_t len, size_t *out_len)
{
    if (len < 10 || memcmp(buf, "ID3", 3)) return NULL;
    int ver = buf[3], flags = buf[5];
    size_t tag = syncsafe(buf + 6);
    if (ver < 2 || ver > 4 || tag + 10 > len) return NULL;
    uint8_t *t = buf + 10;
    if ((flags & 0x80) && ver < 4) tag = id3_unsync(t, tag);   /* whole-tag unsynchronisation (v2.2 / v2.3) */
    size_t pos = 0;
    if ((flags & 0x40) && ver >= 3 && tag >= 4) pos = ver == 4 ? syncsafe(t) : be32(t) + 4;   /* extended header */
    const uint8_t *best = NULL; size_t best_len = 0; int best_front = 0;
    int hl = ver == 2 ? 6 : 10;
    while (pos + hl <= tag && t[pos]) {
        uint8_t *h = t + pos;
        size_t fs = ver == 2 ? ((size_t)h[3] << 16 | (size_t)h[4] << 8 | h[5]) : ver == 4 ? syncsafe(h + 4) : be32(h + 4);
        if (fs == 0 || pos + hl + fs > tag) break;
        uint8_t *d = h + hl;
        size_t dn = fs;
        int is_pic = ver == 2 ? !memcmp(h, "PIC", 3) : !memcmp(h, "APIC", 4);
        if (is_pic) {
            if (ver == 4) {
                if (h[9] & 0x01) { d += 4; dn = dn >= 4 ? dn - 4 : 0; }   /* data length indicator */
                if (h[9] & 0x02) dn = id3_unsync(d, dn);                  /* frame unsynchronisation */
            }
            if (dn > 4) {
                int enc = d[0];
                size_t q = 1;
                if (ver == 2) q += 3;                                     /* image format, e.g. "JPG" */
                else q += id3_skip_str(d + q, dn - q, 0);                 /* MIME type */
                int type = q < dn ? d[q] : 0;
                q++;
                if (q < dn) q += id3_skip_str(d + q, dn - q, enc);        /* description */
                if (q < dn && (!best || (type == 3 && !best_front))) { best = d + q; best_len = dn - q; best_front = type == 3; }
            }
        }
        pos += hl + fs;
    }
    *out_len = best_len;
    return best;
}

int image_load_to_cache(const char *path, VideoCache *vc, int max_w, int max_h, char *err, size_t errlen)
{
    size_t len = 0;
    uint8_t *buf = file_read_all(path, &len, 0);
    if (!buf) { snprintf(err, errlen, "can't read the file"); return -1; }
    const uint8_t *pic = buf;
    size_t pic_len = len;
    if (str_iends(path, ".mp3")) {   /* an MP3 as a picture: its cover art */
        pic = id3_cover(buf, len, &pic_len);
        if (!pic) { free(buf); snprintf(err, errlen, "this MP3 has no cover picture (ID3v2)"); return -1; }
    }
    int w = 0, h = 0, ch = 0;
    uint8_t *px = stbi_load_from_memory(pic, (int)pic_len, &w, &h, &ch, 4);
    free(buf);
    if (!px) { snprintf(err, errlen, "not a picture (%s)", stbi_failure_reason()); return -1; }
    double s = 1.0;
    if (w > max_w) s = (double)max_w / w;
    if (h * s > max_h) s = (double)max_h / h;
    int cw = (int)(w * s + 0.5), chh = (int)(h * s + 0.5);
    if (cw < 1) cw = 1;
    if (chh < 1) chh = 1;
    vc->img = calloc(1, sizeof(GpuImage));
    vc->pts = calloc(1, sizeof(float));
    vc->slot = calloc(1, sizeof(int));
    if (!vc->img || !vc->pts || !vc->slot || gpu_image_alloc(&vc->img[0], cw, chh, 4) < 0) {
        stbi_image_free(px);
        snprintf(err, errlen, "out of GPU memory for the picture");
        return -1;
    }
    uint8_t *scratch = malloc(downscale_scratch_size(w, h, cw, chh));
    if (!scratch) { stbi_image_free(px); snprintf(err, errlen, "out of memory"); return -1; }
    downscale_rgba(px, w * 4, w, h, &vc->img[0], 1, scratch);
    vc->imgq = vc->want_q ? calloc(1, sizeof(GpuImage)) : NULL;
    if (vc->imgq && gpu_image_alloc(&vc->imgq[0], QUARTER(cw), QUARTER(chh), 4) == 0)
        downscale_rgba(px, w * 4, w, h, &vc->imgq[0], 1, scratch);
    free(scratch);
    stbi_image_free(px);
    vc->nimg = 1; vc->nframes = 1; vc->pts[0] = 0; vc->slot[0] = 0;
    vc->still = 1; vc->src_w = w; vc->src_h = h; vc->duration = 1e9;
    vc->cache_w = cw; vc->cache_h = chh; vc->bpp = 4;
    vc->ok = 1;
    return 0;
}
