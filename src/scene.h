/* scene.h - the compositor. A builder thread turns "timeline time T" into a list of GPU layers (texture, corner
 * positions after Pan/Crop + Track Motion, colour matrix of the effect chain, opacity, blend mode); the render thread
 * only submits those quads to sceGxm. Scenes are triple-buffered between the two threads. */
#ifndef SCENE_H
#define SCENE_H

#include "gpu.h"

#define MAX_LAYERS 96
#define MAX_STAGES 48          /* effect stages drawn into render targets, per scene */

typedef struct {
    double t;
    int n;
    GpuLayer layer[MAX_LAYERS];
    GpuLayer stage[MAX_STAGES]; int nstage;   /* see GpuLayer.st0 */
    float vx, vy, vw, vh;      /* video viewport on screen */
    int cpu_fx;                /* layers whose effects were done on the CPU this time */
    int nlut, nblur;           /* layers drawn with the gradient-map / blur shader */
    int nshadow;               /* Track Motion shadow / glow layers */
    uint64_t build_us;
} Scene;

int scene_start(void);         /* spawn the builder (after prepare) */
void scene_stop(void);
const Scene *scene_acquire(void);   /* render thread: newest finished scene (stays valid until the next acquire) */
/* render thread, before the frame's vita2d_start_drawing: draws the layers' earlier effect stages into render targets
 * and points the layers at the results. Returns 1 if it drew (the viewport must then be set again). */
int scene_prepass(const Scene *sc);
void scene_set_viewport(float x, float y, float w, float h);
/* offline rendering (video export), main thread, builder stopped: set up once, then build the scene at any time T */
void scene_offline_begin(void);
const Scene *scene_build_now(double T);

#endif
