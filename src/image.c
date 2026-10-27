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

int image_load_to_cache(const char *path, VideoCache *vc, int max_w, int max_h, char *err, size_t errlen)
{
    size_t len = 0;
    uint8_t *buf = file_read_all(path, &len, 0);
    if (!buf) { snprintf(err, errlen, "can't read the file"); return -1; }
    int w = 0, h = 0, ch = 0;
    uint8_t *px = stbi_load_from_memory(buf, (int)len, &w, &h, &ch, 4);
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
    vc->imgq = g_cfg.small_copies ? calloc(1, sizeof(GpuImage)) : NULL;
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
