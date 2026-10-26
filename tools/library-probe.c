/*
 * library-probe: read-only on-TV check of TizGB's real app/library.c.
 *
 * Runs the exact scanner the app uses (built with TIZGB_DATA_DIR set to
 * /tmp/tizgb-libprobe, so its "internal" root is a throw-away test tree and
 * never the app's data folder) and prints what it finds: the Storage API's
 * storage list and the external roots it chose, every ROM with its location,
 * the scan statistics and limits, and (dev, inode) of each given folder so
 * the ARM stat64 layout can be compared with stat(1). Opens files read-only,
 * writes nothing, shows nothing on screen.
 *
 * usage: library-probe [folder ...]
 */
#include "library.h"

extern int printf(const char *, ...);

void gbp_diag(const char *line) { printf("diag: %s\n", line); }

int main(int argc, char **argv)
{
    lib_scan();
    const lib_stats *st = lib_last_stats();
    for (int r = 0; r < lib_root_count(); r++)
        printf("root %d external=%d present=%d path=%s\n", r, lib_root_is_external(r), lib_root_present(r), lib_root_path(r));
    for (int i = 0; i < lib_count(); i++) {
        const lib_rom *rom = lib_get(i);
        printf("rom %d cgb=0x%02x root=%d where=%s path=%s\n", i, rom->cgb, rom->root, rom->where, rom->path);
    }
    printf("stats roms=%d external_roms=%d roots=%d external_roots=%d dirs=%d entries=%d loops_skipped=%d symlinks_skipped=%d limits=0x%02x storage_rc=%d\n",
           st->roms, st->external_roms, st->roots, st->external_roots, st->dirs, st->entries,
           st->loops_skipped, st->symlinks_skipped, st->limits, st->storage_rc);
    for (int i = 1; i < argc; i++) {
        unsigned long long dev = 0, ino = 0;
        int rc = lib_dir_identity(argv[i], &dev, &ino);
        printf("identity rc=%d dev=%llu ino=%llu path=%s\n", rc, dev, ino, argv[i]);
    }
    printf("mounts_changed_first=%d mounts_changed_again=%d roots_changed=%d\n",
           lib_mounts_changed(), lib_mounts_changed(), lib_roots_changed());
    return 0;
}
