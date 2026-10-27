/* common.h - shared helpers for the Vita build */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

#define APP_NAME    "VegPlayer"
#define APP_DIR     "ux0:data/vegplayer"
#define APP_LOG     APP_DIR "/log.txt"
#define APP_CFG     APP_DIR "/config.txt"
#define SHADER_DIR  APP_DIR "/shaders"

#define SCREEN_W 960
#define SCREEN_H 544
#define OUT_RATE 48000          /* mixer output rate (native for the Vita's audio ports) */

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

void log_init(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) log_printf(__VA_ARGS__)

uint64_t time_us(void);
void sleep_us(unsigned us);

/* files */
int file_exists(const char *path);
int dir_exists(const char *path);
void mkdir_p(const char *path);
uint8_t *file_read_all(const char *path, size_t *len, size_t align);   /* align 0 = malloc */
int64_t file_size(const char *path);
int str_ieq(const char *a, const char *b);
int str_iends(const char *s, const char *suffix);
void path_dir(const char *path, char *out, size_t outsz);              /* "ux0:a/b/c.veg" -> "ux0:a/b" */

/* tiny spinlock-free primitives built on Vita kernel objects */
typedef struct { int id; } Mutex;
void mutex_init(Mutex *m, const char *name);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
void mutex_destroy(Mutex *m);

int thread_start(const char *name, int (*fn)(unsigned, void *), void *arg, int priority, int stack, int cpu_mask);
void thread_join(int thid);

/* settings that the user can change in the options screen (persisted in config.txt) */
typedef struct {
    int force_stretch;        /* -1 = as in the project, else VEG_STRETCH_* */
    int pitch_plugin_mode;    /* -1 = as in the project, 0 = resample, 1 = classic (preserve duration) */
    int apply_event_level;    /* use the event gain / opacity value */
    int show_fps;
    int cache_mb;             /* frame cache budget, MB */
    int compositing_guess;    /* apply the tracks' compositing modes (Add, Multiply, Lighten, ...) */
    int tm_follow_source;     /* scale Track Motion moves with a source of another shape (Vegas doesn't) */
    int small_copies;         /* keep quarter-size frame copies for layers shown small (more memory) */
    int tm_fx;                /* draw Track Motion 2D Shadow / 2D Glow */
    int glow;                 /* draw the Glow video effect */
    char last_dir[512];
} Config;
extern Config g_cfg;
void config_load(void);
void config_save(void);

#endif
