/*
 * library.c - TizGB ROM library scanner (see library.h).
 *
 * Freestanding C like the rest of the app: no libc headers, every external
 * symbol hand-declared and resolved against the TV's libc.so.6 and
 * libstorage.so.0.1. The same file is built into the TPK, the host harness
 * (harness/ui_test.c) and the on-TV read-only probe (tools/library-probe.c).
 *
 * Roots: the app data folder (TIZGB_DATA_DIR, internal) plus every storage
 * the Tizen Storage API reports as external (type 1 external or 2 extended
 * internal) and mounted (state 0 read-write, 1 read-only). The mount path
 * comes from the API; nothing assumes /opt/media/USBDriveA1. Only when the
 * API call itself fails are the folders under TIZGB_MEDIA_FALLBACK used.
 *
 * Traversal is bounded so a malformed or huge drive cannot hang the app or
 * exhaust memory:
 *   - at most LIB_MAX_DEPTH (8) folder levels below each root. Real ROM
 *     collections nest about 4-6 deep (ROMs/Nintendo/Game Boy Color/Europe/
 *     A-M/...); 8 leaves headroom, while anything deeper on a pendrive is
 *     almost always a copied system tree, a backup, or a filesystem loop;
 *   - symbolic links are never followed (d_type DT_LNK is skipped and every
 *     open uses O_NOFOLLOW, so DT_UNKNOWN entries cannot sneak one in);
 *   - every folder's (st_dev, st_ino) is remembered; a folder reached twice
 *     (bind mount, the same drive reported under two paths) is scanned once;
 *   - global budgets per scan: LIB_MAX_DIRS folders, LIB_MAX_ENTRIES
 *     directory entries, LIB_MAX_ROMS results, LIB_TIME_BUDGET_NS wall time,
 *     LIB_PATH_MAX path length. Hitting one stops (or trims) the scan with
 *     what was found so far and reports which limit it was. A corrupted FAT
 *     directory that points back at its parent gives fresh inode numbers at
 *     every level, so for that case the depth and folder budgets are the
 *     guard, not the visited set;
 *   - hidden entries (".*": macOS "._name" AppleDouble files, .Trashes,
 *     .Spotlight-V100) and Windows/Android system folders are skipped; on
 *     the internal root the app's own "saves" folder is skipped.
 * Stack use is bounded too: one 4 KiB getdents buffer per level, 9 levels.
 */
#include "library.h"

extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern int   close(int);
extern long  syscall(long, ...);
extern int  *__errno_location(void);
extern int   snprintf(char *, unsigned long, const char *, ...);
extern unsigned long strlen(const char *);
extern int   strcmp(const char *, const char *);
extern int   strcasecmp(const char *, const char *);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memmove(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
struct lib_timespec { long tv_sec; long tv_nsec; };
extern int   clock_gettime(int, struct lib_timespec *);
extern int   storage_foreach_device_supported(unsigned char (*)(int, int, int, const char *, void *), void *);
extern void  gbp_diag(const char *);

#ifndef TIZGB_DATA_DIR
#define TIZGB_DATA_DIR "/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data"
#endif
#ifndef TIZGB_MOUNTS
#define TIZGB_MOUNTS "/proc/self/mounts"
#endif
/* Used only when the Storage API call fails (the 1.0.0 fallback). */
#ifndef TIZGB_MEDIA_FALLBACK1
#define TIZGB_MEDIA_FALLBACK1 "/opt/media"
#endif
#ifndef TIZGB_MEDIA_FALLBACK2
#define TIZGB_MEDIA_FALLBACK2 "/run/media/owner"
#endif

/* ARM EABI values (the harness translates them for x86-64). */
#define O_RDONLY     0
#define O_NONBLOCK   0x800
#define O_DIRECTORY  0x4000
#define O_NOFOLLOW   0x8000
#define CLOCK_MONOTONIC 1
#define SYS_getdents64 217            /* ARM EABI and x86-64 alike */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10
/* fstat64 and the offset of st_ino in the kernel's struct stat64. ARM:
   st_dev u64 @0, __pad0[4], __st_ino u32 @12, ..., st_ino u64 @96 (size
   104). x86-64 (host harness): fstat, st_dev @0, st_ino @8. */
#if defined(__arm__)
#define SYS_FSTAT64   197
#define STAT_INO_OFF   96
#elif defined(__x86_64__)
#define SYS_FSTAT64     5
#define STAT_INO_OFF    8
#else
#error "library.c: unknown architecture for fstat64"
#endif

typedef struct { unsigned long long dev, ino; } dir_id;

static lib_rom g_roms[LIB_MAX_ROMS];
static int     g_order[LIB_MAX_ROMS];         /* sorted index -> g_roms slot */
static int     g_count;
static char    g_roots[LIB_MAX_ROOTS][LIB_PATH_MAX];
static unsigned char g_root_ext[LIB_MAX_ROOTS];
static unsigned long long g_root_msig[LIB_MAX_ROOTS];   /* its mount-table line at scan time */
static int     g_nroots;
static dir_id  g_seen[LIB_MAX_DIRS];
static int     g_nseen;
static lib_stats g_st;
static unsigned long long g_t0;
static int     g_stop;
static unsigned long long g_mount_sig;
static int     g_mount_sig_valid;

/* ------------------------------------------------------------------ util */
static unsigned long long now_ns(void)
{
    struct lib_timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static void copy_str(char *d, const char *s, int cap)
{
    int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

static int is_rom_name(const char *s)
{
    unsigned long n = strlen(s);
    if (n >= 4 && s[n-3] == '.' && (s[n-2]|32) == 'g' && (s[n-1]|32) == 'b') return 1;
    if (n >= 5 && s[n-4] == '.' && (s[n-3]|32) == 'g' && (s[n-2]|32) == 'b' && (s[n-1]|32) == 'c') return 1;
    if (n >= 5 && s[n-4] == '.' && (s[n-3]|32) == 'c' && (s[n-2]|32) == 'g' && (s[n-1]|32) == 'b') return 1;
    return 0;
}

/* Folders that never hold games but can be large or unreadable. */
static int skip_dir(const char *name, int depth, int root)
{
    if (name[0] == '.') return 1;
    if (depth == 0 && !g_root_ext[root] && !strcmp(name, "saves")) return 1;
    return !strcasecmp(name, "System Volume Information") || !strcasecmp(name, "$RECYCLE.BIN") ||
           !strcasecmp(name, "RECYCLER") || !strcasecmp(name, "LOST.DIR");
}

static void limit_hit(int flag, const char *where)
{
    static const char *names[] = { "roms", "dirs", "entries", "time", "depth", "path" };
    if (g_st.limits & flag) return;
    g_st.limits |= flag;
    int bit = 0; while (bit < 6 && !((1 << bit) & flag)) bit++;
    char line[LIB_PATH_MAX + 64];
    snprintf(line, sizeof line, "library-limit %s at=%.400s", bit < 6 ? names[bit] : "?", where);
    gbp_diag(line);
    if (flag & (LIB_LIMIT_ROMS | LIB_LIMIT_DIRS | LIB_LIMIT_ENTRIES | LIB_LIMIT_TIME)) g_stop = 1;
}

static int over_time(const char *where)
{
    if (now_ns() - g_t0 > LIB_TIME_BUDGET_NS) { limit_hit(LIB_LIMIT_TIME, where); return 1; }
    return 0;
}

int lib_dir_identity_fd(int fd, unsigned long long *dev, unsigned long long *ino)
{
    unsigned long long st[32];                 /* 256 bytes >= struct stat64 / stat */
    memset(st, 0, sizeof st);
    if (syscall(SYS_FSTAT64, fd, st) != 0) return -1;
    memcpy(dev, (char *)st, 8);
    memcpy(ino, (char *)st + STAT_INO_OFF, 8);
    return *ino ? 0 : -1;                      /* 0 = layout not trusted: no dedupe */
}

int lib_dir_identity(const char *path, unsigned long long *dev, unsigned long long *ino)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    int rc = lib_dir_identity_fd(fd, dev, ino);
    close(fd);
    return rc;
}

/* ------------------------------------------------------------ results */
static int rom_before(const lib_rom *a, const lib_rom *b)
{
    int c = name_cmp(a->name, b->name);
    if (!c) c = name_cmp(a->where, b->where);
    if (!c) c = strcmp(a->path, b->path);
    return c < 0;
}

/* cgb flag from the header; -1 if this is not a readable ROM-sized file. */
static int probe_header(const char *path)
{
    unsigned char h[0x150];
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return -1;
    long n = read(fd, h, sizeof h);
    close(fd);
    if (n < (long)sizeof h) return -1;
    return h[0x143];
}

static void make_where(char *out, int cap, int root, const char *rel)
{
    const char *r = g_roots[root], *base = r;
    if (g_root_ext[root]) { for (const char *p = r; *p; p++) if (*p == '/' && p[1]) base = p + 1; }
    else base = "TizGB data";
    snprintf(out, (unsigned long)cap, "%s%s%s", base, rel[0] ? "/" : "", rel);
}

/* 1 = added, 0 = not a ROM (caller may try it as a folder). */
static int add_rom(const char *path, const char *name, int root, const char *rel)
{
    if (g_count >= LIB_MAX_ROMS) { limit_hit(LIB_LIMIT_ROMS, path); return 1; }
    int cgb = probe_header(path);
    if (cgb < 0) return 0;
    lib_rom *r = &g_roms[g_count];
    copy_str(r->path, path, sizeof r->path);
    copy_str(r->name, name, sizeof r->name);
    make_where(r->where, sizeof r->where, root, rel);
    r->cgb = (unsigned char)cgb;
    r->root = (unsigned char)root;
    int lo = 0, hi = g_count;                  /* binary insertion into g_order */
    while (lo < hi) { int mid = (lo + hi) / 2; if (rom_before(r, &g_roms[g_order[mid]])) hi = mid; else lo = mid + 1; }
    memmove(&g_order[lo + 1], &g_order[lo], (unsigned long)(g_count - lo) * sizeof g_order[0]);
    g_order[lo] = g_count;
    g_count++;
    g_st.roms++;
    if (cgb & 0x80) g_st.color++;
    if (g_root_ext[root]) g_st.external_roms++;
    return 1;
}

/* ----------------------------------------------------------- traversal */
/* 1 if the folder was opened and scanned (0: unreadable, already visited,
   or a budget ran out). */
static int scan_dir(const char *dir, int depth, int root, const char *rel)
{
    if (g_stop || over_time(dir)) return 0;
    if (g_st.dirs >= LIB_MAX_DIRS) { limit_hit(LIB_LIMIT_DIRS, dir); return 0; }
    /* A root comes from the Storage API or is the app's own folder and may
       itself be a link; nothing below it is followed. */
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NONBLOCK | (depth ? O_NOFOLLOW : 0));
    if (fd < 0) {
        if (depth == 0) {
            char line[LIB_PATH_MAX + 48];
            snprintf(line, sizeof line, "library-root-unreadable errno=%d path=%.400s", *__errno_location(), dir);
            gbp_diag(line);
        }
        return 0;
    }
    dir_id id;
    if (lib_dir_identity_fd(fd, &id.dev, &id.ino) == 0) {
        for (int i = 0; i < g_nseen; i++)
            if (g_seen[i].dev == id.dev && g_seen[i].ino == id.ino) {
                char line[LIB_PATH_MAX + 48];
                snprintf(line, sizeof line, "library-skip already-visited path=%.400s", dir);
                gbp_diag(line);
                g_st.loops_skipped++;
                close(fd);
                return 0;
            }
        if (g_nseen < LIB_MAX_DIRS) g_seen[g_nseen++] = id;
    }
    g_st.dirs++;
    char buf[4096];
    for (;;) {
        long nread = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (nread < 0) {
            char line[LIB_PATH_MAX + 48];
            snprintf(line, sizeof line, "library-read-error errno=%d path=%.400s", *__errno_location(), dir);
            gbp_diag(line);
            break;
        }
        if (nread == 0) break;
        long off = 0;
        while (off < nread) {
            /* linux_dirent64: d_ino(8) d_off(8) d_reclen(2)@16 d_type(1)@18 name@19 */
            unsigned short reclen = *(unsigned short *)(buf + off + 16);
            unsigned char  dtype  = *(unsigned char  *)(buf + off + 18);
            const char *name = buf + off + 19;
            if (reclen == 0) { off = nread; break; }
            off += reclen;
            if (++g_st.entries > LIB_MAX_ENTRIES) { limit_hit(LIB_LIMIT_ENTRIES, dir); goto done; }
            if ((g_st.entries & 255) == 0 && over_time(dir)) goto done;
            if (name[0] == '.') continue;              /* ., .., hidden, macOS ._ files */
            if (dtype == DT_LNK) { g_st.symlinks_skipped++; continue; }
            if (dtype != DT_DIR && dtype != DT_REG && dtype != DT_UNKNOWN) continue;
            char sub[LIB_PATH_MAX];
            int n = snprintf(sub, sizeof sub, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
            if (n <= 0 || n >= (int)sizeof sub) { limit_hit(LIB_LIMIT_PATH, dir); continue; }
            if (dtype != DT_DIR && is_rom_name(name)) {
                if (add_rom(sub, name, root, rel) || dtype == DT_REG) continue;
            }
            if (dtype == DT_REG) continue;
            if (depth >= LIB_MAX_DEPTH) { if (dtype == DT_DIR) limit_hit(LIB_LIMIT_DEPTH, sub); continue; }
            if (skip_dir(name, depth, root)) continue;
            char subrel[128];
            snprintf(subrel, sizeof subrel, "%s%s%s", rel, rel[0] ? "/" : "", name);
            scan_dir(sub, depth + 1, root, subrel);    /* DT_UNKNOWN non-folders fail O_DIRECTORY */
            if (g_stop) goto done;
        }
    }
done:
    close(fd);
    return 1;
}

/* --------------------------------------------------------------- roots */
typedef struct { char path[LIB_MAX_ROOTS][LIB_PATH_MAX]; int n; int log; } root_set;
static root_set g_collect;

static void set_add(root_set *s, const char *path)
{
    char p[LIB_PATH_MAX];
    copy_str(p, path, sizeof p);
    int n = (int)strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    if (!n) return;
    for (int i = 0; i < s->n; i++) if (!strcmp(s->path[i], p)) return;
    if (s->n < LIB_MAX_ROOTS) copy_str(s->path[s->n++], p, LIB_PATH_MAX);
}

static unsigned char storage_cb(int storage_id, int type, int state, const char *path, void *user_data)
{
    (void)user_data;
    /* STORAGE_TYPE_EXTERNAL 1, EXTENDED_INTERNAL 2; MOUNTED 0, MOUNTED_READ_ONLY 1. */
    int use = (type == 1 || type == 2) && state >= 0 && path && path[0];
    if (g_collect.log) {
        char line[LIB_PATH_MAX + 80];
        snprintf(line, sizeof line, "storage id=%d type=%d state=%d use=%d path=%.400s",
                 storage_id, type, state, use, path ? path : "(null)");
        gbp_diag(line);
    }
    if (use) set_add(&g_collect, path);
    return 1;
}

/* Fallback only: each folder directly inside a media directory is a drive. */
static void collect_fallback(root_set *s, const char *media)
{
    char buf[4096];
    int fd = open(media, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return;
    for (;;) {
        long nread = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (nread <= 0) break;
        for (long off = 0; off < nread;) {
            unsigned short reclen = *(unsigned short *)(buf + off + 16);
            unsigned char dtype = *(unsigned char *)(buf + off + 18);
            const char *name = buf + off + 19;
            if (reclen == 0) break;
            off += reclen;
            if (name[0] == '.' || (dtype != DT_DIR && dtype != DT_UNKNOWN)) continue;
            char p[LIB_PATH_MAX];
            int n = snprintf(p, sizeof p, "%s/%s", media, name);
            if (n > 0 && n < (int)sizeof p) set_add(s, p);
        }
    }
    close(fd);
}

/* External roots now mounted, via the Storage API (fallback if it fails). */
static int collect_external(int log)
{
    g_collect.n = 0; g_collect.log = log;
    int rc = storage_foreach_device_supported(storage_cb, 0);
    if (rc != 0) {
        if (log) {
            char line[96]; snprintf(line, sizeof line, "storage-enumeration rc=%d; using media-folder fallback", rc);
            gbp_diag(line);
        }
        g_collect.n = 0;
        collect_fallback(&g_collect, TIZGB_MEDIA_FALLBACK1);
        collect_fallback(&g_collect, TIZGB_MEDIA_FALLBACK2);
    }
    return rc;
}

/* ---------------------------------------------------------- mount table */
static char g_mbuf[65536];

static long read_mounts(void)
{
    int fd = open(TIZGB_MOUNTS, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    long total = 0;
    while (total < (long)sizeof g_mbuf - 1) {
        long n = read(fd, g_mbuf + total, sizeof g_mbuf - 1 - (unsigned long)total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    g_mbuf[total] = 0;
    return total;
}

static unsigned long long fnv64(const char *p, long n)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (long i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static void mount_sig_update(void)
{
    long n = read_mounts();
    g_mount_sig = n >= 0 ? fnv64(g_mbuf, n) : 0;
    g_mount_sig_valid = n >= 0;
}

int lib_mounts_changed(void)
{
    long n = read_mounts();
    if (n < 0) return 0;
    unsigned long long sig = fnv64(g_mbuf, n);
    if (g_mount_sig_valid && sig == g_mount_sig) return 0;
    int first = !g_mount_sig_valid;
    g_mount_sig = sig; g_mount_sig_valid = 1;
    return !first;
}

/* Signature of the mount-table line whose mount point is `path` (device,
   mount point, type, options): 0 if `path` is not mounted, 1 if the table
   is unreadable. Mount points there escape space, tab, newline and
   backslash as \ooo. */
static unsigned long long mount_line_sig(const char *path)
{
    if (read_mounts() < 0) return 1;
    char *p = g_mbuf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        long len = p - line;
        if (*p) *p++ = 0;
        char *mp = line;
        while (*mp && *mp != ' ') mp++;        /* skip the device field */
        if (!*mp) continue;
        mp++;
        char dec[LIB_PATH_MAX]; int k = 0;
        while (*mp && *mp != ' ' && k < (int)sizeof dec - 1) {
            if (mp[0] == '\\' && mp[1] >= '0' && mp[1] <= '7' && mp[2] >= '0' && mp[2] <= '7' && mp[3] >= '0' && mp[3] <= '7') {
                dec[k++] = (char)(((mp[1] - '0') << 6) | ((mp[2] - '0') << 3) | (mp[3] - '0'));
                mp += 4;
            } else dec[k++] = *mp++;
        }
        dec[k] = 0;
        if (!strcmp(dec, path)) { unsigned long long h = fnv64(line, len); return h > 1 ? h : 2; }
    }
    return 0;
}

int lib_root_present(int root)
{
    if (root < 0 || root >= g_nroots) return 0;
    if (!g_root_ext[root]) return 1;
    unsigned long long sig = mount_line_sig(g_roots[root]);
    if (sig != 1) return sig != 0;
    unsigned long long dev, ino;               /* no mount table: is it still a folder? */
    return lib_dir_identity(g_roots[root], &dev, &ino) == 0;
}

int lib_roots_changed(void)
{
    collect_external(0);
    int n = 0;
    for (int i = 0; i < g_nroots; i++) if (g_root_ext[i]) n++;
    if (n != g_collect.n) return 1;
    for (int i = 0; i < g_collect.n; i++) {
        int found = 0;
        for (int j = 0; j < g_nroots && !found; j++) if (g_root_ext[j] && !strcmp(g_roots[j], g_collect.path[i])) found = 1;
        if (!found) return 1;
    }
    /* Same mount points, but a different drive (or remount) on one of them. */
    for (int j = 0; j < g_nroots; j++)
        if (g_root_ext[j] && mount_line_sig(g_roots[j]) != g_root_msig[j]) return 1;
    return 0;
}

/* ---------------------------------------------------------------- scan */
static void add_root(const char *path, int external)
{
    for (int i = 0; i < g_nroots; i++) if (!strcmp(g_roots[i], path)) return;
    if (g_nroots >= LIB_MAX_ROOTS) return;
    copy_str(g_roots[g_nroots], path, LIB_PATH_MAX);
    g_root_ext[g_nroots] = (unsigned char)external;
    g_root_msig[g_nroots] = external ? mount_line_sig(path) : 0;
    g_nroots++;
}

void lib_scan(void)
{
    g_count = 0; g_nroots = 0; g_nseen = 0; g_stop = 0;
    memset(&g_st, 0, sizeof g_st);
    g_t0 = now_ns();
    mount_sig_update();                        /* changes after this point trigger a rescan */
    add_root(TIZGB_DATA_DIR, 0);
    g_st.storage_rc = collect_external(1);
    for (int i = 0; i < g_collect.n; i++) add_root(g_collect.path[i], 1);
    for (int r = 0; r < g_nroots && !g_stop; r++) {
        char line[LIB_PATH_MAX + 48];
        snprintf(line, sizeof line, "library-root n=%d external=%d path=%.400s", r, g_root_ext[r], g_roots[r]);
        gbp_diag(line);
        /* counted only when scanned: a drive reported twice is one drive */
        if (scan_dir(g_roots[r], 0, r, "")) { g_st.roots++; if (g_root_ext[r]) g_st.external_roots++; }
    }
    g_st.elapsed_ns = now_ns() - g_t0;
    char line[320];
    snprintf(line, sizeof line,
        "library roms=%d color=%d mono=%d external_roms=%d roots=%d external_roots=%d dirs=%d entries=%d "
        "loops_skipped=%d symlinks_skipped=%d limits=0x%02x elapsed_ms=%lu storage_rc=%d",
        g_st.roms, g_st.color, g_st.roms - g_st.color, g_st.external_roms, g_st.roots, g_st.external_roots,
        g_st.dirs, g_st.entries, g_st.loops_skipped, g_st.symlinks_skipped, g_st.limits,
        (unsigned long)(g_st.elapsed_ns / 1000000ULL), g_st.storage_rc);
    gbp_diag(line);
}

int lib_count(void) { return g_count; }
const lib_rom *lib_get(int i) { return (i >= 0 && i < g_count) ? &g_roms[g_order[i]] : 0; }
const lib_stats *lib_last_stats(void) { return &g_st; }
int lib_root_count(void) { return g_nroots; }
const char *lib_root_path(int r) { return (r >= 0 && r < g_nroots) ? g_roots[r] : ""; }
int lib_root_is_external(int r) { return (r >= 0 && r < g_nroots) ? g_root_ext[r] : 0; }
int lib_find_path(const char *path)
{
    if (!path || !path[0]) return -1;
    for (int i = 0; i < g_count; i++) if (!strcmp(g_roms[g_order[i]].path, path)) return i;
    return -1;
}
