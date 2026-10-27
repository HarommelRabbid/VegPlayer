/* export.h - render the project's mix offline to a WAV or MP3 file (a background thread; the UI polls g_export) */
#ifndef EXPORT_H
#define EXPORT_H

#include <stdint.h>

enum { EXPORT_WAV = 0, EXPORT_MP3 };

typedef struct {
    volatile int running, done, cancel, failed;
    volatile float progress;       /* 0..1 */
    int fmt, kbps;
    char path[512];
    char msg[160];                 /* error or result text */
    uint64_t t0, t1;               /* time_us() at start / end */
} ExportJob;

extern ExportJob g_export;

/* fmt EXPORT_WAV / EXPORT_MP3 (kbps: 128, 192, 256, 320). The session must be prepared and playback paused. */
int export_start(int fmt, int kbps);
void export_finish(void);          /* join the thread after done */
const char *export_dir(void);

#endif
