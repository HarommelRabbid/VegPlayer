/* veg.h - Vegas Pro .veg project reader (portable C, no Vita dependencies).
 *
 * Derived from the .veg readers in vegtool.c and tved.c (GUIDs, Pan/Crop keyframes, generators, project
 * settings), rewritten to walk the chunk tree structurally and extended with the parts a real-time player needs:
 *   - media pool entries by id (full UTF-16 paths, so "Vidéo" folders survive),
 *   - per-event audio time-stretch / pitch settings (method élastique / Classic / None, semitones),
 *   - event level (audio gain / video opacity) and audio fades,
 *   - legacy DXT video effects (Invert, Black and White) and OFX effects (HSL Adjust, Invert, ...),
 *   - the DirectX audio plug-in chain (Pitch Shift, Vibrato).
 * The format is reverse-engineered, so everything here is best effort.
 */
#ifndef VEG_H
#define VEG_H

#include <stddef.h>
#include <stdint.h>

#define VEG_TICKS 1e7 /* Vegas 8+ store times in 100 ns ticks */

enum { VEG_KIND_AUDIO = 0, VEG_KIND_VIDEO = 1, VEG_KIND_OTHER = 2 };

/* time-stretch / pitch methods of an audio event (Event Properties > Time stretch/pitch shift) */
enum { VEG_STRETCH_NONE = 0, VEG_STRETCH_CLASSIC = 1, VEG_STRETCH_ELASTIQUE = 2 };

typedef struct { double t, w, h, cx, cy, rot; } VegPanKey; /* t: seconds from event start; window in source pixels */

/* video effects. Every supported one is an affine colour transform, so a chain folds into one 3x4 matrix. */
enum {
    VFX_UNKNOWN = 0,
    VFX_INVERT,      /* amount 0..1 */
    VFX_GRAYSCALE,   /* Black and White: amount 0..1 */
    VFX_HSL,         /* HSL Adjust: hue (turns), saturation (x), luminance (x) */
    VFX_SEPIA,       /* amount 0..1 */
    VFX_BRICON,      /* Brightness and Contrast: brightness (-1..1), contrast (-1..1) */
};
typedef struct {
    int type;
    char name[96];      /* plug-in id / preset, for reports */
    double amount;
    double hue, sat, lum;
    double bright, contrast;
} VegVfx;

/* audio plug-ins */
enum { AFX_UNKNOWN = 0, AFX_PITCH_SHIFT, AFX_VIBRATO };
typedef struct {
    int type;
    char name[96];
    double semitones;   /* Pitch Shift: total shift (semitones + cents) */
    int preserve;       /* Pitch Shift: 1 = preserve duration (Classic stretch), 0 = resample */
    int mode;           /* Pitch Shift: Classic mode index when preserving duration */
    double rate_hz;     /* Vibrato */
    double depth;       /* Vibrato: depth in semitones */
} VegAfx;

typedef struct {
    int track;          /* index into VegProject.tracks */
    int kind;           /* VEG_KIND_* */
    int seq;            /* file order */
    double start, length;   /* timeline seconds */
    double rate;            /* playback rate (1 = normal) */
    double offset;          /* take in-point as stored (source position = (offset + t) * rate) */
    int has_take;
    uint32_t media_id;
    int media;              /* index into VegProject.media, -1 if none */
    char take_name[160];
    float level;            /* audio gain / video opacity (1 = 100%) */
    double fade_in, fade_out; /* seconds (audio and video events) */
    int fade_in_curve, fade_out_curve; /* VEG_CURVE_* */
    double xfade_in, xfade_out; /* overlap with the previous / next event on the same track (crossfade) */
    /* audio time stretch / pitch */
    int has_stretch, stretch_method, stretch_attr;
    double pitch_semis;
    /* video */
    VegPanKey *pan; int npan; int pan_w, pan_h;
    VegVfx *vfx; int nvfx;
    /* audio */
    VegAfx *afx; int nafx;
} VegEvent;

/* fade curve types, as Vegas stores them (event chunk @0x74 fade in, @0x78 fade out) */
enum { VEG_CURVE_LINEAR = 1, VEG_CURVE_FAST = 2, VEG_CURVE_SLOW = -2, VEG_CURVE_SMOOTH = 4, VEG_CURVE_SHARP = -4 };
/* gain 0..1 along a fade-in at progress u (0..1); a fade-out at progress u has gain 1 - veg_fade_shape(curve, u) */
float veg_fade_shape(int curve, double u);

typedef struct {
    int kind;              /* VEG_KIND_AUDIO / VEG_KIND_VIDEO (from the track header; OTHER if unknown) */
    int first_event, nevents;   /* events of this track are events[first_event .. first_event+nevents) sorted by start */
    int has_motion; double tm_x, tm_y, tm_w, tm_h;  /* Track Motion (first keyframe) */
    int has_comp; uint32_t comp_a; int32_t comp_b;  /* compositing mode (codes partly guessed) */
    float level;           /* video track opacity / composite level (1 = 100%) */
    VegVfx *vfx; int nvfx;  /* track-level video FX */
    VegAfx *afx; int nafx;  /* track-level audio FX */
} VegTrack;

enum { VEG_MEDIA_FILE = 0, VEG_MEDIA_SOLID, VEG_MEDIA_TEXT, VEG_MEDIA_NOISE, VEG_MEDIA_GENERATOR };
typedef struct {
    uint32_t id;
    int type;              /* VEG_MEDIA_* */
    char path[1024];       /* path stored in the project (UTF-8, Windows style) */
    char name[256];        /* file name without directories */
    int streams;           /* stream count stored with the media (2 = video+audio, ...) */
    int used_video, used_audio;  /* number of video / audio events that use this media */
    float rgba[4];         /* solid colour generator */
    double use_lo[2], use_hi[2]; /* [0]=video [1]=audio: source time range the events need (seconds, before looping) */
} VegMedia;

typedef struct {
    double fps; int width, height; int sample_rate;
    double duration;
    VegTrack *tracks; int ntracks;
    VegEvent *events; int nevents;
    VegMedia *media; int nmedia;
    int n_unknown_vfx, n_unknown_afx;
    char unknown_fx[1024];  /* short list of effect names we parse but can't play */
} VegProject;

/* Parse a whole .veg already in memory. Returns 0 on success; errbuf receives a message on failure. */
int veg_parse(const uint8_t *data, size_t len, VegProject *p, char *errbuf, size_t errlen);
/* Read a file and parse it (uses stdio). */
int veg_load_file(const char *path, VegProject *p, char *errbuf, size_t errlen);
void veg_free(VegProject *p);

/* Index of the first event of track t whose end is after time `when` (binary search), or track end. */
int veg_track_seek(const VegProject *p, const VegTrack *t, double when);

/* helpers shared with the player */
const char *veg_basename(const char *path);   /* after the last '\\' or '/' */
const char *veg_ext(const char *path);        /* ".mp4" or "" */

#endif
