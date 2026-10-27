/* pfs.h - read official (PFS-encrypted) games and apps, like VitaShell's "Open decrypted": a title folder
 * (<device>:app/TITLEID or <device>:patch/TITLEID) is mounted with sceAppMgrGameDataMount, after which its files
 * read decrypted at their usual paths for this process. One title at a time. */
#ifndef PFS_H
#define PFS_H

#include <stddef.h>

/* if path is inside an encrypted title folder, make sure that title is mounted (unmounting another).
 * Returns 2 = mounted now, 1 = was already, 0 = not an encrypted title (nothing to do), < 0 = the mount failed (SCE error). */
int pfs_ensure_for(const char *path);
void pfs_unmount(void);
const char *pfs_mounted_title(void);   /* "PCSB00000" or "" */
/* the title folder of path ("ux0:app/PCSB00000"), if it is inside one; 0 if not */
int pfs_title_root(const char *path, char *root, size_t n);

#endif
