/* scene.h - the compositor. A builder thread turns "timeline time T" into a list of GPU layers (texture, corner
 * positions after Pan/Crop + Track Motion, colour matrix of the effect chain, opacity, blend mode); the render thread
 * only submits those quads to sceGxm. Scenes are triple-buffered between the two threads. */
#ifndef SCENE_H
#define SCENE_H

#include "gpu.h"

#define MAX_LAYERS 96

typedef struct {
    double t;
    int n;
    GpuLayer layer[MAX_LAYERS];
    float vx, vy, vw, vh;      /* video viewport on screen */
    int cpu_fx;                /* layers whose effects were done on the CPU this time */
    int nlut, nblur;           /* layers drawn with the gradient-map / blur shader */
    uint64_t build_us;
} Scene;

int scene_start(void);         /* spawn the builder (after prepare) */
void scene_stop(void);
const Scene *scene_acquire(void);   /* render thread: newest finished scene (stays valid until the next acquire) */
void scene_set_viewport(float x, float y, float w, float h);

#endif
