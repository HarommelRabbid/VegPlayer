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

/* keyframe interpolation to the next keyframe (the player's own numbering; veg_key_type() converts what Vegas stores) */
enum { VEG_KEY_LINEAR = 0, VEG_KEY_FAST, VEG_KEY_SLOW, VEG_KEY_SMOOTH, VEG_KEY_SHARP, VEG_KEY_HOLD };
/* Vegas's stored keyframe type (Pan/Crop keyframe @16, Track Motion keyframe @4) -> VEG_KEY_*. Stored: 0 Linear, 1 Hold, 2 Slow,
 * 3 Fast, 4 Smooth, 5 Sharp - all checked in VEGAS Pro 11 (test projects with one keyframe of each type, for Pan/Crop and for Track Motion). */
int veg_key_type(uint32_t stored);
typedef struct { double t, w, h, cx, cy, rot; int curve; } VegPanKey; /* t: seconds from event start; window in source pixels */

/* video effects. Most are affine colour transforms and fold into one 3x4 matrix; Gradient Map (a colour lookup
 * on brightness) and the blurs need their own GPU pass. */
enum {
    VFX_UNKNOWN = 0,
    VFX_INVERT,      /* amount 0..1 */
    VFX_GRAYSCALE,   /* Black and White: amount 0..1 */
    VFX_HSL,         /* HSL Adjust: hue (turns), saturation (x), luminance (x) */
    VFX_SEPIA,       /* amount 0..1 */
    VFX_BRICON,      /* Brightness and Contrast: brightness (-1..1), contrast (-1..1) */
    VFX_MATRIX,      /* Channel Blend: mtx (rows R,G,B: r, g, b, offset) */
    VFX_LAB,         /* LAB Adjust: lum (lightness x), a/b offsets, invert, desat */
    VFX_COLORCORR,   /* Color Corrector (saturation, gain, offset) and Color Balance (offsets in mtx[3,7,11]) */
    VFX_GRADIENT,    /* Gradient Map: brightness -> colour stops, mixed by amount */
    VFX_LINEARBLUR,  /* Linear Blur: angle (degrees), amount (fraction of the picture) */
    VFX_RADIALBLUR,  /* Radial Blur: amount, centre (cx, cy 0..1) */
    VFX_NOOP,        /* recognised, nothing to do (e.g. default Color Corrector) */
    VFX_GAUSSBLUR,   /* Gaussian Blur: a = horizontal range, b = vertical range (fractions of the picture) */
    VFX_THRESHOLD,   /* Threshold: each channel 0 or 1 at amount (0..1) */
    VFX_SECCC,       /* Color Corrector (Secondary): prm = see veg.c */
    VFX_DEFORM,      /* Deform: prm[0..13] as stored (amount, flag, -, -, shear x/y, squeeze t/b/l/r, curve t/b/l/r) */
    VFX_SWIRL,       /* Swirl: prm = amount, centre x/y, scale x/y */
    VFX_WAVE,        /* Wave: prm = frequency x/y, amplitude x/y, phase x/y */
    VFX_COOKIE,      /* Cookie Cutter: see veg.c */
    VFX_SPHERIZE,    /* Spherize: prm = amount, centre x/y (texcoords), scale x/y */
    VFX_PINCH,       /* Pinch/Punch: prm = amount, centre x/y (texcoords), scale x/y */
    VFX_LAST = VFX_PINCH
};
#define VFX_MAX_STOPS 8
typedef struct {
    int type;
    char name[96];      /* plug-in id / preset, for reports */
    double amount;
    double hue, sat, lum;
    double bright, contrast;
    double a, b; int invert, desat;   /* LAB Adjust */
    double angle, cx, cy;             /* blurs */
    float mtx[12];                    /* VFX_MATRIX / VFX_COLORCORR */
    int nstops; float stop_pos[VFX_MAX_STOPS], stop_rgb[VFX_MAX_STOPS][3];   /* Gradient Map */
    int lut;                          /* player side: gradient texture slot (-1 = none) */
    float prm[24]; int ip[4];         /* the effects above from VFX_THRESHOLD on */
    struct VegVfxKeys *keys;          /* animated: the effect's settings at each keyframe (NULL if not animated) */
} VegVfx;

/* an animated effect: n snapshots (keys[i] has the effect's settings at time t[i], seconds from the event's start; for
 * track effects from the project's start) and the interpolation to the next one (VEG_KEY_*) */
typedef struct VegVfxKeys { int n; double *t; int *curve; VegVfx *v; int in_frames; } VegVfxKeys;
/* the effect at time t: v itself if it isn't animated, else the interpolated settings in *tmp */
const VegVfx *veg_vfx_at(const VegVfx *v, double t, VegVfx *tmp);
/* frees the keyframes of n effects (not the array) */
void veg_vfx_free_keys(VegVfx *v, int n);

/* track compositing modes (Vegas's CompositeMode order; the track compositing chunk @0x64) */
enum {
    VEG_COMP_CUSTOM = -1, VEG_COMP_SRCALPHA3D = 0, VEG_COMP_ADD, VEG_COMP_SUBTRACT, VEG_COMP_CUT, VEG_COMP_SRCALPHA,
    VEG_COMP_MULTIPLY, VEG_COMP_OVERLAY, VEG_COMP_SCREEN, VEG_COMP_HARDLIGHT, VEG_COMP_DODGE, VEG_COMP_BURN,
    VEG_COMP_DARKEN, VEG_COMP_LIGHTEN, VEG_COMP_DIFFERENCE, VEG_COMP_DIFFSQUARED,
};
const char *veg_comp_name(int mode);

/* audio plug-ins */
enum { AFX_UNKNOWN = 0, AFX_PITCH_SHIFT, AFX_VIBRATO, AFX_REVERB, AFX_NOOP };
typedef struct {
    int type;
    char name[96];
    double semitones;   /* Pitch Shift: total shift (semitones + cents) */
    int preserve;       /* Pitch Shift: 1 = preserve duration (Classic stretch), 0 = resample */
    int mode;           /* Pitch Shift: Classic mode index when preserving duration */
    /* Vibrato: a pitch curve drawn over one cycle, repeated at rate_hz from the event's start; the curve's points
     * (position 0..1 in the cycle, height -1..1) times `range` semitones; past the last point the pitch is 0.
     * VEGAS resamples (the source plays faster / slower, no time stretch). gain_db: output gain. */
    double rate_hz, range, gain_db;
    int npts;
    float px[32], py[32];
    /* Reverb (Sony Reverb, track FX): gains in dB, decay in seconds, pre-delay in ms, high cut in Hz (0 = off) */
    double dry_db, wet_db, early_db, decay_s, predelay_ms, hicut_hz;
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

/* Track Motion 2D Shadow / 2D Glow: a soft, coloured copy of the track's shape drawn behind it. Position and
 * size are relative to the track's frame (the same units as Track Motion: x, y offset; w, h scale, 1 = same size). */
typedef struct {
    int on;
    double x, y, w, h;
    float rgba[4];         /* colour (stored BGRA) and its alpha */
    double blur;           /* softness, in the same units as the position */
} VegTmFx;

/* a Track Motion keyframe: t in seconds of the project timeline; x, y position; w, h size (Track Motion units);
 * rot in radians (counter-clockwise); curve VEG_KEY_* (the way to the next keyframe). 3D: z depth (away from the
 * viewer), orientation quaternion q (w, x, y, z; x right, y up, z towards the viewer) */
typedef struct { double t, x, y, w, h, rot; int curve; double z, q[4]; } VegTmKey;

typedef struct {
    int kind;              /* VEG_KIND_AUDIO / VEG_KIND_VIDEO (from the track header; OTHER if unknown) */
    int first_event, nevents;   /* events of this track are events[first_event .. first_event+nevents) sorted by start */
    int has_motion; double tm_x, tm_y, tm_w, tm_h;  /* Track Motion (first keyframe) */
    VegTmKey *tmk; int ntmk;   /* Track Motion position keyframes (time-sorted; ntmk >= 1 when has_motion) */
    VegTmFx shadow, glow;  /* Track Motion 2D Shadow / 2D Glow (first keyframe; .on = enabled) */
    int tm3d;              /* the keyframes leave the plane (depth, turns about x / y) */
    double tm_base;        /* Track Motion's unit width (the frame's half width; its height is 1): usually the frame's
                            * aspect, the picture's when the track was made for it (0: the frame's aspect) */
    VegTmKey *pmk; int npmk;   /* a compositing parent's Parent Motion (applied to its children) */
    double pm_base;        /* the same, for Parent Motion (1: its sizes are scale factors) */
    int parent;            /* video track: index of its compositing parent, -1 if none */
    int has_comp; uint32_t comp_a; int32_t comp_b;  /* compositing chunk words @0x60 / @0x64 */
    uint32_t comp_flags;   /* compositing chunk @0x0c: 0x40 compositing parent, 0x20 compositing child */
    int comp_mode;         /* VEG_COMP_* (Source Alpha when absent) */
    float level;           /* video track: opacity (Level); audio track: volume, linear gain (1 = 0 dB); the UI can change it */
    float level_veg;       /* the project's own value (the sliders' reset) */
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
    /* from the project settings chunk: where the project was saved, and the editor that saved it (its settings
     * folder, e.g. "...\AppData\Local\Sony\Vegas Pro\11.0\") */
    char saved_path[512];
    char app_dir[512];
    char app_name[96];      /* e.g. "Vegas Pro 11.0 (Sony)", "VEGAS Pro 20.0", "Movie Studio Platinum 12.0 (Sony)" */
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
