/* pfs.c - see pfs.h */
#include "pfs.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "common.h"

#ifdef __vita__
#include <psp2/appmgr.h>
#include <psp2/io/stat.h>
#endif

static char g_root[64];        /* the mounted title folder */
static char g_mp[16];          /* its mount point (for unmounting) */
static char g_title[16];

const char *pfs_mounted_title(void) { return g_title; }

/* "<dev>:app/TITLEID..." or "<dev>:patch/TITLEID..." -> "<dev>:app/TITLEID" (kind 1) or "<dev>:patch/TITLEID" (kind 2) */
int pfs_title_root(const char *path, char *root, size_t n)
{
    const char *c = strchr(path, ':');
    if (!c || c - path > 8) return 0;
    const char *r = c + 1;
    while (*r == '/') r++;
    int kind = !strncasecmp(r, "app/", 4) ? 1 : !strncasecmp(r, "patch/", 6) ? 2 : 0;
    if (!kind) return 0;
    const char *id = r + (kind == 1 ? 4 : 6);
    int len = 0;
    while (id[len] && id[len] != '/') len++;
    if (len != 9) return 0;                       /* title IDs: 4 letters + 5 digits */
    for (int i = 0; i < 9; i++) if (!isalnum((unsigned char)id[i])) return 0;
    snprintf(root, n, "%.*s", (int)(id + 9 - path), path);
    return kind;
}

void pfs_unmount(void)
{
#ifdef __vita__
    if (g_mp[0]) {
        int r = sceAppMgrUmount(g_mp);
        LOG("pfs: unmounted %s (%s): 0x%08X", g_root, g_mp, r);
    }
#endif
    g_root[0] = g_mp[0] = g_title[0] = 0;
}

int pfs_ensure_for(const char *path)
{
    char root[64];
    int kind = pfs_title_root(path, root, sizeof root);
    if (!kind) return 0;
    if (g_root[0] && !strcasecmp(root, g_root)) return 1;
#ifdef __vita__
    char probe[96];
    SceIoStat st;
    snprintf(probe, sizeof probe, "%s/sce_pfs", root);
    if (sceIoGetstat(probe, &st) < 0) return 0;   /* not encrypted (homebrew, system app): readable as it is */
    pfs_unmount();
    char mp[16] = "";
    int r = sceAppMgrGameDataMount(root, NULL, NULL, mp);
    if (r < 0 && kind == 2) {
        /* a patch folder: mount the game with its patch, or the game alone (VitaShell does the latter) */
        const char *id = root + strlen(root) - 9;
        char dev[16], app[64];
        snprintf(dev, sizeof dev, "%.*s", (int)(strchr(root, ':') - root), root);
        snprintf(app, sizeof app, "%s:app/%s", dev, id);
        memset(mp, 0, sizeof mp);
        r = sceAppMgrGameDataMount(app, root, NULL, mp);
        if (r < 0) { memset(mp, 0, sizeof mp); r = sceAppMgrGameDataMount(app, NULL, NULL, mp); }
    }
    LOG("pfs: mount %s -> %s: 0x%08X", root, mp, r);
    if (r < 0) return r;
    snprintf(g_root, sizeof g_root, "%s", root);
    snprintf(g_mp, sizeof g_mp, "%s", mp);
    snprintf(g_title, sizeof g_title, "%s", root + strlen(root) - 9);
    return 2;
#else
    return 0;
#endif
}
