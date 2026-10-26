/*
 * library.h - TizGB ROM library (library.c): finds .gb/.gbc/.cgb files in
 * the app data folder and on every mounted external drive that the Tizen
 * Storage API reports, and watches the mount table for drive changes.
 * Shared by the app (gb_main.c), the host harness and tools/library-probe.c.
 */
#ifndef TIZGB_LIBRARY_H
#define TIZGB_LIBRARY_H

#define LIB_MAX_ROMS      2048    /* entries kept (a complete GB + GBC set is ~2500 incl. variants) */
#define LIB_PATH_MAX       512    /* longer paths are skipped, never truncated */
#define LIB_MAX_DEPTH        8    /* folder levels below a drive / data root */
#define LIB_MAX_DIRS      4096    /* folders opened per scan, all roots together */
#define LIB_MAX_ENTRIES 200000    /* directory entries examined per scan */
#define LIB_TIME_BUDGET_NS 10000000000ULL   /* 10 s wall time per scan */
#define LIB_MAX_ROOTS       16

/* lib_stats.limits bits: which budget stopped or trimmed the scan. */
#define LIB_LIMIT_ROMS     0x01
#define LIB_LIMIT_DIRS     0x02
#define LIB_LIMIT_ENTRIES  0x04
#define LIB_LIMIT_TIME     0x08
#define LIB_LIMIT_DEPTH    0x10   /* informational: folders deeper than LIB_MAX_DEPTH were not entered */
#define LIB_LIMIT_PATH     0x20   /* informational: over-long paths skipped */

typedef struct {
    char path[LIB_PATH_MAX];     /* full path; the ROM is loaded straight from here */
    char name[128];              /* file name */
    char where[128];             /* "USBDriveA1/Pokemon/Gen 2", "TizGB data/roms" */
    unsigned char cgb;           /* header byte 0x143 */
    unsigned char root;          /* index for lib_root_*() */
} lib_rom;

typedef struct {
    int roms, color, external_roms;
    int roots, external_roots;
    int dirs, entries, loops_skipped, symlinks_skipped;
    int limits;                  /* LIB_LIMIT_* */
    int storage_rc;              /* storage_foreach_device_supported() result */
    unsigned long long elapsed_ns;
} lib_stats;

void lib_scan(void);                         /* full rescan of every root */
int  lib_count(void);
const lib_rom *lib_get(int index);           /* sorted by name, then location */
int  lib_find_path(const char *path);        /* sorted index, or -1 */
const lib_stats *lib_last_stats(void);
int  lib_root_count(void);
const char *lib_root_path(int root);
int  lib_root_is_external(int root);
int  lib_root_present(int root);             /* external root still mounted */

/* Hot-plug: cheap mount-table check (call every ~2 s), then a Storage API
   re-enumeration that says whether the set of mounted drives differs from
   the one the last scan used. */
int  lib_mounts_changed(void);
int  lib_roots_changed(void);

/* (dev, inode) of a directory; 0 on success. Used for loop detection and by
   the on-TV probe to check the stat64 layout against stat(1). */
int  lib_dir_identity(const char *path, unsigned long long *dev, unsigned long long *ino);

#endif
