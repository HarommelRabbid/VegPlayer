/* gpu.h - sceGxm side of the player: GPU memory arenas for the frame caches, the effect shaders (compiled at
 * first run with vitaShaRK and cached as .gxp), and the per-layer draw used by the compositor. */
#ifndef GPU_H
#define GPU_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/gxm.h>

/* MULTIPLY/SCREEN/LIGHTEN expect the colour premultiplied by the opacity, DARKEN mixed towards white (scene.c) */
/* BLEND_COPY: writes colour and alpha as they are (effect stages drawn into a render target) */
enum { BLEND_NORMAL = 0, BLEND_ADD, BLEND_SUBTRACT, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_LIGHTEN, BLEND_DARKEN, BLEND_CUT, BLEND_COPY, BLEND_COUNT };
/* layer passes: colour matrix; matrix -> brightness lookup (Gradient Map) -> matrix; blur along a line / towards a centre */
/* PASS_SHADOW: the picture's shape (its alpha) with softened edges, filled with the tint colour (Track Motion shadow/glow) */
/* PASS_HSL / PASS_THRESH / PASS_SECCC: matrix -> a non-linear colour effect (HSL Adjust, Threshold, Secondary Color
 * Corrector) -> matrix m2. PASS_SWIRL / PASS_WAVE / PASS_DEFORM: where each pixel reads from in the picture, then the
 * matrix. PASS_COOKIE: Cookie Cutter's mask (shape, border, feather, repeats), then the matrix. Parameters in fx[]. */
enum { PASS_MATRIX = 0, PASS_LUT, PASS_BLUR, PASS_SHADOW, PASS_HSL, PASS_THRESH, PASS_SECCC, PASS_SWIRL, PASS_WAVE,
       PASS_DEFORM, PASS_COOKIE, PASS_COUNT };

/* ---- memory: big mapped blocks carved into textures (CDRAM first, then main RAM) */
size_t gpu_arena_reserve(size_t want);       /* main thread; returns bytes actually reserved */
void gpu_arena_release(void);                /* main thread, GPU idle */
void *gpu_arena_alloc(size_t size, size_t align);   /* any thread */
size_t gpu_arena_capacity(void);
size_t gpu_arena_used(void);

typedef struct {
    SceGxmTexture tex;
    void *data;
    int w, h, stride;   /* stride in pixels (multiple of 8) */
    int bpp;            /* 2 (RGB565) or 4 (RGBA8888) */
} GpuImage;

/* allocate pixels from the arena and set up a linear, bilinear-filtered texture */
int gpu_image_alloc(GpuImage *img, int w, int h, int bpp);
/* (re)initialise the texture descriptor for pixels already owned elsewhere */
int gpu_image_wrap(GpuImage *img, void *data, int w, int h, int bpp);

/* ---- shaders and drawing (render thread only, between vita2d_start_drawing/end_drawing) */
int gpu_init(void);            /* after vita2d_init; tries cached .gxp, then vitaShaRK; falls back to vita2d programs */
void gpu_fini(void);
int gpu_shaders_ok(void);      /* 1 if the effect shader is available (effects on the GPU) */
int gpu_pass_ok(int pass);     /* PASS_LUT / PASS_BLUR shaders available */
const char *gpu_status(void);  /* human-readable shader status */

typedef struct {
    const SceGxmTexture *tex;
    float x[4], y[4];   /* screen-space corners of the source picture: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
    float u[4], v[4];   /* texcoords of those corners (0..1; beyond for a blur that spills past the picture) */
    float m[12];        /* colour matrix rows R,G,B: (r, g, b, constant) */
    float tint[4];      /* final multiplier (r, g, b, a) */
    int blend;          /* BLEND_* */
    int fx;             /* m is not the identity */
    int pass;           /* PASS_* */
    float m2[12];       /* PASS_LUT: matrix after the lookup */
    const SceGxmTexture *lut;   /* PASS_LUT: 256x1 gradient */
    float lutp[4];      /* PASS_LUT: amount, texel scale, texel offset */
    float dir[4];       /* PASS_BLUR: step (texcoords, at the outermost tap) and radial centre */
    float rad[4];       /* PASS_BLUR: x = radial strength */
    float w[20];        /* PASS_BLUR: 17 tap weights (sum 1) */
    float soft[4];      /* PASS_SHADOW: 1 / (2 * edge), edge (texcoords, x and y); the quad is enlarged by edge */
    int st0, nst;       /* effect stages drawn into render targets before this layer (Scene.stage[st0 ..]) */
    int rt_w, rt_h;     /* their size (the picture's) */
    float fxp[16];      /* PASS_HSL .. PASS_COOKIE: uniforms uFx0..uFx3 (see gpu.c) */
    int tex_prev;       /* another piece of the layer before it (a picture cut to its Pan/Crop window): same texture */
} GpuLayer;

void gpu_draw_layer(const GpuLayer *L);
/* the same, with the vertices in gpu.c's own buffer instead of vita2d's pool (render-target passes drawn before the
 * frame's scene starts: vita2d_start_drawing resets the pool while the GPU may still read them); call
 * gpu_own_vertices_reset() once per frame, with the GPU idle */
void gpu_draw_layer_own(const GpuLayer *L);
void gpu_own_vertices_reset(void);

/* colour-matrix helpers (shared with the CPU fallback) */
void cm_identity(float m[12]);
void cm_mul(float out[12], const float a[12], const float b[12]);   /* out = a after b */
int cm_is_identity(const float m[12]);

#endif
