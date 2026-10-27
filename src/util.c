/* util.c - logging, files, threads, config (Vita) */
#include "common.h"

#include <ctype.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

Config g_cfg = { -1, -1, 1, 1, 0, 1, 1, "" };

static SceUID g_log_mutex = -1;

void log_init(void)
{
    mkdir_p(APP_DIR);
    g_log_mutex = sceKernelCreateMutex("vp_log", 0, 0, NULL);
    SceUID fd = sceIoOpen(APP_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) sceIoClose(fd);
}

void log_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    if (n == 0 || buf[n - 1] != '\n') buf[n++] = '\n';
    buf[n] = 0;
    if (g_log_mutex >= 0) sceKernelLockMutex(g_log_mutex, 1, NULL);
    SceUID fd = sceIoOpen(APP_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, buf, (SceSize)n); sceIoClose(fd); }
    if (g_log_mutex >= 0) sceKernelUnlockMutex(g_log_mutex, 1);
}

uint64_t time_us(void) { return (uint64_t)sceKernelGetProcessTimeWide(); }
void sleep_us(unsigned us) { sceKernelDelayThread(us); }

int file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0 && !SCE_S_ISDIR(st.st_mode);
}

int dir_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = strchr(tmp, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        if (strchr(tmp, ':') && tmp[strlen(tmp) - 1] != ':') sceIoMkdir(tmp, 0777);
        *p = '/';
    }
    sceIoMkdir(tmp, 0777);
}

int64_t file_size(const char *path)
{
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0) return -1;
    return (int64_t)st.st_size;
}

uint8_t *file_read_all(const char *path, size_t *len, size_t align)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > 512ll * 1024 * 1024) { sceIoClose(fd); return NULL; }
    size_t n = (size_t)sz;
    uint8_t *b = align ? memalign(align, (n + align - 1) & ~(align - 1)) : malloc(n + 1);
    if (!b) { sceIoClose(fd); return NULL; }
    size_t got = 0;
    while (got < n) {
        int r = sceIoRead(fd, b + got, (SceSize)MIN(n - got, (size_t)(4 << 20)));
        if (r <= 0) break;
        got += (size_t)r;
    }
    sceIoClose(fd);
    if (got != n) { free(b); return NULL; }
    if (align) memset(b + n, 0, ((n + align - 1) & ~(align - 1)) - n);   /* aligned buffers: zero the padding */
    *len = n;
    return b;
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == 0 && *b == 0;
}

int str_iends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && str_ieq(s + a - b, suf);
}

void path_dir(const char *path, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", path);
    char *s = strrchr(out, '/');
    if (s) *s = 0;
    else { char *c = strchr(out, ':'); if (c) c[1] = 0; }
}

void mutex_init(Mutex *m, const char *name) { m->id = sceKernelCreateMutex(name, 0, 0, NULL); }
void mutex_lock(Mutex *m) { sceKernelLockMutex(m->id, 1, NULL); }
void mutex_unlock(Mutex *m) { sceKernelUnlockMutex(m->id, 1); }
void mutex_destroy(Mutex *m) { if (m->id >= 0) sceKernelDeleteMutex(m->id); m->id = -1; }

typedef struct { int (*fn)(unsigned, void *); void *arg; } ThreadBox;

static int thread_tramp(SceSize args, void *argp)
{
    ThreadBox *tb = *(ThreadBox **)argp;
    (void)args;
    int r = tb->fn(0, tb->arg);
    free(tb);
    sceKernelExitDeleteThread(r);
    return r;
}

int thread_start(const char *name, int (*fn)(unsigned, void *), void *arg, int priority, int stack, int cpu_mask)
{
    ThreadBox *tb = malloc(sizeof *tb);
    if (!tb) return -1;
    tb->fn = fn; tb->arg = arg;
    SceUID th = sceKernelCreateThread(name, thread_tramp, priority, stack, 0, cpu_mask, NULL);
    if (th < 0) { free(tb); LOG("thread %s: create failed 0x%08X", name, th); return -1; }
    if (sceKernelStartThread(th, sizeof(tb), &tb) < 0) { free(tb); sceKernelDeleteThread(th); return -1; }
    return th;
}

void thread_join(int thid)
{
    if (thid < 0) return;
    int status = 0;
    SceUInt timeout = 0;
    (void)timeout;
    sceKernelWaitThreadEnd(thid, &status, NULL);
}

/* ------------------------------------------------------------------ config */

void config_load(void)
{
    FILE *f = fopen(APP_CFG, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        char *nl = strpbrk(eq, "\r\n");
        if (nl) *nl = 0;
        if (!strcmp(line, "force_stretch")) g_cfg.force_stretch = atoi(eq);
        else if (!strcmp(line, "pitch_plugin_mode")) g_cfg.pitch_plugin_mode = atoi(eq);
        else if (!strcmp(line, "apply_event_level")) g_cfg.apply_event_level = atoi(eq);
        else if (!strcmp(line, "show_fps")) g_cfg.show_fps = atoi(eq);
        else if (!strcmp(line, "cache_mb")) g_cfg.cache_mb = atoi(eq);
        else if (!strcmp(line, "compositing_guess")) g_cfg.compositing_guess = atoi(eq);
        else if (!strcmp(line, "tm_follow_source")) g_cfg.tm_follow_source = atoi(eq);
        else if (!strcmp(line, "last_dir")) snprintf(g_cfg.last_dir, sizeof g_cfg.last_dir, "%s", eq);
    }
    fclose(f);
}

void config_save(void)
{
    mkdir_p(APP_DIR);
    FILE *f = fopen(APP_CFG, "w");
    if (!f) return;
    fprintf(f, "force_stretch=%d\npitch_plugin_mode=%d\napply_event_level=%d\nshow_fps=%d\ncache_mb=%d\ncompositing_guess=%d\ntm_follow_source=%d\nlast_dir=%s\n",
            g_cfg.force_stretch, g_cfg.pitch_plugin_mode, g_cfg.apply_event_level, g_cfg.show_fps, g_cfg.cache_mb,
            g_cfg.compositing_guess, g_cfg.tm_follow_source, g_cfg.last_dir);
    fclose(f);
}
