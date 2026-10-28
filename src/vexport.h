/* vexport.h - export the project as an MP4 video (H.264 + AAC) with the system's MP4 recorder (SceMp4Rec, the
 * library behind the system's own clip recording). The picture is rendered offline on the GPU, frame by frame, on the
 * main thread; the recorder encodes on its own threads and saves the file to ux0:video. */
#ifndef VEXPORT_H
#define VEXPORT_H

#include <stdint.h>

/* the recorder's modes (SceMp4RecMode): resolution and video bitrate are fixed per mode, 29.97 fps, 48 kHz AAC */
typedef struct { int w, h, kbps; } VExportMode;
#define VEXPORT_NMODES 6
extern const VExportMode VEXPORT_MODES[VEXPORT_NMODES];

typedef struct {
    int running, cancel, failed;
    int mode;
    int frame, nframes;            /* video frames done / total */
    float progress;                /* 0..1 */
    uint64_t t0;
    char msg[256];                 /* result or error */
    void *preview;                 /* vita2d_texture * of the last rendered frame (for the progress screen) */
} VExport;

extern VExport g_vexp;

/* main thread; the session must be prepared and playback stopped. Returns 0 if the recorder started. */
int vexport_start(int mode);
/* main thread, once per UI frame BEFORE vita2d_start_drawing: renders and encodes the next frame (and the sound
 * up to it); when everything is done (or cancelled) it closes the file and sets running = 0 */
void vexport_step(void);

#endif
