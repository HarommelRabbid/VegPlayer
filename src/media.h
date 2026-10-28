/* media.h - what the player keeps for each source: a frame cache in GPU memory for pictures, decoded PCM for sound */
#ifndef MEDIA_H
#define MEDIA_H

#include <stdint.h>
#include "gpu.h"
#include "veg.h"

typedef struct {
    int ok;
    int still;              /* picture file: one frame */
    int src_w, src_h;       /* size of the original picture (placement math uses this) */
    double duration;        /* seconds, for looping */
    int nframes;            /* display-order frames of the source */
    float *pts;             /* nframes presentation times (seconds), ascending */
    int *slot;              /* per frame: index into img, or -1 */
    GpuImage *img; int nimg;
    GpuImage *imgq;         /* per slot: a quarter-size copy (or NULL): small layers sample it, faster and no shimmer */
    int cache_w, cache_h, bpp;
    int want_q;             /* make the quarter-size copies (the small-layer option) */
} VideoCache;

typedef struct { int64_t start; int64_t count; int16_t *pcm; } PcmSeg;   /* stereo interleaved, positions in source frames */
typedef struct {
    int ok;
    int rate;               /* source sample rate */
    int64_t total;          /* source length in frames (for looping) */
    PcmSeg *seg; int nseg;
} AudioCache;

enum { MS_NONE = 0, MS_AUTO, MS_USER, MS_SKIP };   /* how a source file got chosen */

typedef struct {
    char vfile[512], afile[512];   /* files used for the picture / the sound of this media ("" = none) */
    int vstate, astate;            /* MS_* */
    VideoCache vc;
    AudioCache ac;
    char vmsg[128], amsg[128];     /* status lines for the UI */
    volatile float vprog, aprog;   /* 0..1 while preparing */
} MediaRt;

typedef struct {
    VegProject proj;
    MediaRt *m;                    /* proj.nmedia entries */
    char veg_path[512], veg_dir[512];
    volatile int *track_on;        /* proj.ntracks toggles (changed live from the UI) */
    GpuImage white;                /* 1x1 white for solid colours */
    volatile float master;         /* master volume (linear; the tracks panel's first row) */
    int prepared;
} Session;

extern Session g_s;

/* ---- loader (prepare.c) */
int session_open(const char *veg_path, char *err, size_t errlen);
void session_close(void);
void session_auto_find_sources(void);                  /* look next to the .veg for files with the right names */
void session_save_sources(void);                       /* "<project>.veg.vegplayer" next to the project */
int media_kind_of_file(const char *path);              /* bit 1 = has video/picture, bit 2 = has audio (by extension) */

typedef struct {
    volatile int running, done, cancel;
    volatile float progress;
    char line[160];
} PrepareState;
extern PrepareState g_prep;
int prepare_start(void);                               /* spawns the loader threads */
void prepare_finish(void);                             /* joins them (after g_prep.done) */
void session_free_caches(void);

/* ---- decoders */
/* video: hardware H.264 (sceAvcdec) into the frame cache. need[k] = 1 for display frames to keep. */
int vdec_global_init(int max_w, int max_h, int max_refs);
void vdec_global_term(void);
struct Mp4File;
int vdec_fill_cache(struct Mp4File *mf, VideoCache *vc, const uint8_t *need, volatile float *progress, volatile int *cancel, char *err, size_t errlen);

/* audio: decode into PCM segments covering `ranges` (source frames, sorted, disjoint) */
typedef struct AudioSrc AudioSrc;
void adec_global_init(void);
void adec_global_term(void);
int adec_open(const char *path, AudioSrc **out, char *err, size_t errlen);
void adec_info(AudioSrc *a, int *rate, int64_t *total_frames);
int adec_decode(AudioSrc *a, AudioCache *ac, const int64_t (*ranges)[2], int nranges, volatile float *progress, volatile int *cancel,
                char *err, size_t errlen);
void adec_close(AudioSrc *a);
const char *adec_codec_name(AudioSrc *a);

/* image files (stb_image) */
int image_load_to_cache(const char *path, VideoCache *vc, int max_w, int max_h, char *err, size_t errlen);

/* shared downscaler: RGBA8888 rows -> cache image (box filter) */
void downscale_rgba(const uint8_t *src, int src_pitch_bytes, int sw, int sh, GpuImage *dst, int keep_alpha, uint8_t *scratch);
#define QUARTER(n) ((n) / 4 > 8 ? (n) / 4 : 8)   /* size of the quarter-size copy */
size_t downscale_scratch_size(int sw, int sh, int dw, int dh);

#endif
