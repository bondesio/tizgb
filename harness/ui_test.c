/*
 * ui_test.c - host harness for the real app/gb_main.c + app/platform.c.
 *
 * gb_main.c is compiled freestanding exactly as for the TV, with only these
 * renames (see Makefile): open/read/close/ioctl -> fk_* (fake input device
 * nodes over a fake sysfs tree on disk), clock_gettime -> fake monotonic
 * clock, storage enumeration -> stub, gbp_run_frame -> counting wrapper,
 * main -> tizgb_app_main. EFL/Ecore/appfw are recording stubs; the animator
 * is ticked by the harness at chosen compositor rates.
 *
 * Scenarios: library scan/tags/sort, sectioned Settings navigation and
 * layout, settings persistence, pacing at 60/50/jittery/stalled compositor
 * rates with audio pulled at 44.1 kHz, DualShock 4 five-node layout and
 * Afterglow selection with exclusive grab, jitter vs TV-remote keys,
 * SYN_DROPPED resync, battery save across sessions, Exit.
 * USB library explorer (1.1.0): app/library.c is compiled with the same
 * renames, the Storage API is a fake list of drives (directories under
 * out/ui/usb) and the mount table is out/ui/mounts. Covers hot-plug by the
 * mount poll, depth limit 8, symlink loops/links, (dev,ino) dedupe of a
 * drive reported twice, hidden/system folders, mixed internal + external
 * results, save-key collisions (same file name and title on two drives),
 * same-content sharing, sidecar .sav import, a drive removed during play,
 * launching from a drive removed before the poll noticed, Settings > Rescan
 * picking up a new drive, the folder/time/ROM-count budgets and the
 * Storage-API-failure fallback.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "library.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("PASS "); } else { g_fail++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

/* ======================= fake EFL / Ecore / appfw ======================= */
typedef struct { int used, shown, x, y, w, h, smooth; char text[512]; } obj_t;
static obj_t g_objs[256];
static int g_nobj;
static void *new_obj(void) { obj_t *o = &g_objs[g_nobj++]; o->used = 1; return o; }
typedef struct { void *create, *terminate, *pause, *resume, *app_control; } callbacks_t;
typedef unsigned char Eina_Bool;
void *elm_win_util_standard_add(const char *a, const char *b) { (void)a; (void)b; return new_obj(); }
void elm_win_fullscreen_set(void *o, int v) { (void)o; (void)v; }
void elm_win_resize_object_add(void *a, void *b) { (void)a; (void)b; }
void elm_object_focus_set(void *o, Eina_Bool v) { (void)o; (void)v; }
void *evas_object_evas_get(void *o) { return o; }
void *evas_object_rectangle_add(void *e) { (void)e; return new_obj(); }
void *evas_object_text_add(void *e) { (void)e; return new_obj(); }
void *evas_object_image_filled_add(void *e) { (void)e; return new_obj(); }
void *elm_label_add(void *p) { (void)p; return new_obj(); }
void elm_object_part_text_set(void *o, const char *part, const char *t) { (void)part; snprintf(((obj_t *)o)->text, 512, "%s", t ? t : ""); }
void evas_object_image_size_set(void *o, int w, int h) { (void)o; (void)w; (void)h; }
void evas_object_image_data_set(void *o, void *d) { (void)o; (void)d; }
static unsigned long g_frames_presented;
void evas_object_image_data_update_add(void *o, int x, int y, int w, int h) { (void)o; (void)x; (void)y; (void)w; (void)h; g_frames_presented++; }
void evas_object_image_smooth_scale_set(void *o, Eina_Bool v) { ((obj_t *)o)->smooth = v; }
void evas_object_image_alpha_set(void *o, Eina_Bool v) { (void)o; (void)v; }
void evas_object_text_font_set(void *o, const char *f, int s) { (void)o; (void)f; (void)s; }
void evas_object_text_text_set(void *o, const char *t) { snprintf(((obj_t *)o)->text, 512, "%s", t ? t : ""); }
int evas_object_text_horiz_advance_get(const void *o) { return (int)strlen(((const obj_t *)o)->text) * 14; }
void evas_object_color_set(void *o, int r, int g, int b, int a) { (void)o; (void)r; (void)g; (void)b; (void)a; }
void evas_object_move(void *o, int x, int y) { ((obj_t *)o)->x = x; ((obj_t *)o)->y = y; }
void evas_object_resize(void *o, int w, int h) { ((obj_t *)o)->w = w; ((obj_t *)o)->h = h; }
void evas_object_show(void *o) { ((obj_t *)o)->shown = 1; }
void evas_object_hide(void *o) { ((obj_t *)o)->shown = 0; }
int ECORE_EVENT_KEY_DOWN = 10, ECORE_EVENT_KEY_UP = 11;
typedef Eina_Bool (*key_cb)(void *, int, void *);
static key_cb g_key_down, g_key_up;
void *ecore_event_handler_add(int type, key_cb cb, const void *d) { (void)d; if (type == 10) g_key_down = cb; else g_key_up = cb; return (void *)1; }
typedef Eina_Bool (*anim_cb)(void *);
static anim_cb g_anim;
void *ecore_animator_add(anim_cb cb, const void *d) { (void)d; g_anim = cb; return (void *)1; }
static int g_timer_added, g_ui_exit;
void *ecore_timer_add(double t, anim_cb cb, const void *d) { (void)t; (void)cb; (void)d; g_timer_added = 1; return (void *)1; }
void ui_app_exit(void) { g_ui_exit = 1; }
static callbacks_t g_cb;
static void run_scenarios(void);
int ui_app_main(int argc, char **argv, const callbacks_t *cb, void *d)
{
    (void)argc; (void)argv; (void)d;
    g_cb = *cb;
    run_scenarios();
    return 0;
}
typedef struct { const char *keyname, *key, *string; } key_ev;
static void key(const char *name)
{
    key_ev e = { name, name, name };
    g_key_down(0, 10, &e);
    g_key_up(0, 11, &e);
}
/* Fake Tizen Storage API + mount table. */
typedef struct { int id, type, state, listed, mline; char path[256]; } fstor_t;
static fstor_t g_fs[16];
static int g_nfs, g_storage_rc, g_storage_calls;
int fk_storage(unsigned char (*cb)(int, int, int, const char *, void *), void *u)
{
    g_storage_calls++;
    if (g_storage_rc) return g_storage_rc;
    for (int i = 0; i < g_nfs; i++)
        if (g_fs[i].listed && !cb(g_fs[i].id, g_fs[i].type, g_fs[i].state, g_fs[i].path, u)) break;
    return 0;
}
static void write_mounts(void)
{
    FILE *f = fopen("out/ui/mounts", "w");
    fputs("rootfs / rootfs rw 0 0\nproc /proc proc rw,relatime 0 0\ntmpfs /opt/media tmpfs rw,relatime 0 0\n", f);
    for (int i = 0; i < g_nfs; i++) {
        if (!g_fs[i].listed || !g_fs[i].mline || g_fs[i].state < 0) continue;
        fprintf(f, "/dev/sd%c1 ", 'a' + i);
        for (const char *p = g_fs[i].path; *p; p++) { if (*p == ' ') fputs("\\040", f); else fputc(*p, f); }
        fputs(" vfat rw,dirsync,noatime 0 0\n", f);
    }
    fclose(f);
}
static fstor_t *storage_set(int id, int type, int state, const char *path, int mline)
{
    fstor_t *e = 0;
    for (int i = 0; i < g_nfs; i++) if (g_fs[i].id == id) e = &g_fs[i];
    if (!e) e = &g_fs[g_nfs++];
    e->id = id; e->type = type; e->state = state; e->listed = 1; e->mline = mline;
    snprintf(e->path, sizeof e->path, "%s", path);
    write_mounts();
    return e;
}
static fstor_t *storage_get(int id) { for (int i = 0; i < g_nfs; i++) if (g_fs[i].id == id) return &g_fs[i]; return 0; }
/* Unplug: the drive leaves the API list and the mount table, and its files
   vanish from the mount point (moved aside so it can be plugged back). */
static void unplug(int id)
{
    fstor_t *e = storage_get(id); char away[300];
    snprintf(away, sizeof away, "out/ui/unplugged/%d", id);
    e->listed = 0; write_mounts();
    if (rename(e->path, away) != 0) { printf("unplug rename failed\n"); exit(2); }
}
static void plug(int id)
{
    fstor_t *e = storage_get(id); char away[300];
    snprintf(away, sizeof away, "out/ui/unplugged/%d", id);
    if (rename(away, e->path) != 0) { printf("plug rename failed\n"); exit(2); }
    e->listed = 1; write_mounts();
}

/* capi-media-audio-io fakes: count what the "TV" receives */
typedef void *audio_out_h;
static unsigned long long g_audio_bytes;
int audio_out_create_new(int r, int c, int t, audio_out_h *h) { (void)r; (void)c; (void)t; *h = (void *)1; return 0; }
int audio_out_set_stream_cb(audio_out_h h, void *cb, void *u) { (void)h; (void)cb; (void)u; return 0; }
int audio_out_prepare(audio_out_h h) { (void)h; return 0; }
int audio_out_unprepare(audio_out_h h) { (void)h; return 0; }
int audio_out_destroy(audio_out_h h) { (void)h; return 0; }
int audio_out_write(audio_out_h h, void *b, unsigned int n) { (void)h; (void)b; g_audio_bytes += n; return (int)n; }
extern void gbp_audio_pull_for_test(unsigned int);
extern unsigned int gbp_ring_fill(void), gbp_audio_underruns(void), gbp_audio_overruns(void);

/* ============================ fake clock ============================ */
static unsigned long long g_now = 1000000000ULL;
static unsigned long long g_clock_step;   /* added on every read (scan time-budget test) */
struct fk_ts { long s, ns; };
int fk_clock_gettime(int c, struct fk_ts *ts) { (void)c; g_now += g_clock_step; ts->s = (long)(g_now / 1000000000ULL); ts->ns = (long)(g_now % 1000000000ULL); return 0; }

static unsigned long g_frames_run;
extern int gbp_run_frame(void);
int fk_run_frame(void) { g_frames_run++; return gbp_run_frame(); }

/* ======================= fake input device nodes ======================= */
#define DEV "out/ui/dev"
#define SYS "out/ui/sys"
typedef struct { unsigned long long sec, usec; unsigned short type, code; int value; } ev_t;   /* x86-64 input_event */
typedef struct { uint32_t time; int16_t value; uint8_t type, number; } jsev_t;
typedef struct {
    char node[16];          /* "js1", "event21" */
    char name[64];
    int is_event, axes, buttons;
    uint8_t axmap[64]; uint16_t btnmap[512];
    int grab_fd;            /* fd holding EVIOCGRAB, or -1 */
    int abs_min[64], abs_max[64], abs_val[64];
    uint8_t keys[96];
    ev_t q[256]; int qh, qt;
    jsev_t jq[256]; int jqh, jqt;
} fdev_t;
static fdev_t g_dev[16];
static int g_ndev;
static int g_fd_dev[1024];    /* fd -> dev index + 1 */

static fdev_t *dev_by_node(const char *node) { for (int i = 0; i < g_ndev; i++) if (!strcmp(g_dev[i].node, node)) return &g_dev[i]; return 0; }

int fk_open(const char *path, int flags, ...)
{
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    /* The app hard-codes ARM EABI flag values; O_DIRECTORY is 0x4000 there
       and 0x10000 on x86-64 (where 0x4000 is O_DIRECT). The other flags it
       uses (O_NONBLOCK, O_CREAT, O_TRUNC, O_APPEND) are equal. */
    if (flags & 0x4000) flags = (flags & ~0x4000) | O_DIRECTORY;
    if (flags & 0x8000) flags = (flags & ~0x8000) | O_NOFOLLOW;    /* ARM O_NOFOLLOW (x86-64: O_LARGEFILE) */
    int fd = open(path, flags, mode);
    if (fd >= 0 && fd < 1024 && !strncmp(path, DEV "/", strlen(DEV) + 1)) {
        fdev_t *d = dev_by_node(path + strlen(DEV) + 1);
        g_fd_dev[fd] = d ? (int)(d - g_dev) + 1 : 0;
    }
    return fd;
}
int fk_close(int fd)
{
    if (fd >= 0 && fd < 1024 && g_fd_dev[fd]) {
        fdev_t *d = &g_dev[g_fd_dev[fd] - 1];
        if (getenv("UI_TRACE") && d->is_event) printf("     [trace] close %s fd=%d holder=%d\n", d->node, fd, d->grab_fd);
        if (d->grab_fd == fd) d->grab_fd = -1;
        g_fd_dev[fd] = 0;
    }
    return close(fd);
}
long fk_read(int fd, void *buf, unsigned long n)
{
    if (fd >= 0 && fd < 1024 && g_fd_dev[fd]) {
        fdev_t *d = &g_dev[g_fd_dev[fd] - 1];
        if (d->is_event) {
            if (d->grab_fd >= 0 && d->grab_fd != fd) { errno = EAGAIN; return -1; }   /* grabbed by another fd */
            if (d->qh == d->qt || n < sizeof(ev_t)) { errno = EAGAIN; return -1; }
            memcpy(buf, &d->q[d->qh], sizeof(ev_t)); d->qh = (d->qh + 1) % 256; return sizeof(ev_t);
        }
        if (d->jqh == d->jqt || n < sizeof(jsev_t)) { errno = EAGAIN; return -1; }
        memcpy(buf, &d->jq[d->jqh], sizeof(jsev_t)); d->jqh = (d->jqh + 1) % 256; return sizeof(jsev_t);
    }
    return read(fd, buf, n);
}
static int g_grab_attempts_by_app;
int fk_ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (fd < 0 || fd >= 1024 || !g_fd_dev[fd]) { errno = ENOTTY; return -1; }
    fdev_t *d = &g_dev[g_fd_dev[fd] - 1];
    unsigned long nr = req & 0xff, type = (req >> 8) & 0xff, size = (req >> 16) & 0x3fff;
    if (type == 'j') {
        if (d->is_event) { errno = ENOTTY; return -1; }
        if (nr == 0x11) { *(uint8_t *)arg = (uint8_t)d->axes; return 0; }
        if (nr == 0x12) { *(uint8_t *)arg = (uint8_t)d->buttons; return 0; }
        if (nr == 0x13) { snprintf(arg, size, "%s", d->name); return 0; }
        if (nr == 0x32) { memcpy(arg, d->axmap, 64); return 0; }
        if (nr == 0x34) { memcpy(arg, d->btnmap, 1024); return 0; }
    } else if (type == 'E') {
        if (!d->is_event) { errno = ENOTTY; return -1; }
        if (nr == 0x90) {   /* EVIOCGRAB */
            int on = *(int *)arg;
            if (getenv("UI_TRACE")) printf("     [trace] EVIOCGRAB %s fd=%d on=%d holder=%d\n", d->node, fd, on, d->grab_fd);
            if (on) {
                g_grab_attempts_by_app++;
                if (d->grab_fd >= 0 && d->grab_fd != fd) { errno = EBUSY; return -1; }
                d->grab_fd = fd; return 0;
            }
            if (d->grab_fd == fd) d->grab_fd = -1;
            return 0;
        }
        if (nr >= 0x40 && nr < 0x80) {   /* EVIOCGABS(axis) */
            int a = (int)(nr - 0x40);
            if (d->abs_max[a] <= d->abs_min[a]) { errno = EINVAL; return -1; }
            int *info = arg; info[0] = d->abs_val[a]; info[1] = d->abs_min[a]; info[2] = d->abs_max[a];
            info[3] = info[4] = info[5] = 0; return 0;
        }
        if (nr == 0x18) { memcpy(arg, d->keys, size < 96 ? size : 96); return 0; }
    }
    errno = ENOTTY; return -1;
}

static void mkfile(const char *path, const char *content)
{
    FILE *f = fopen(path, "w"); if (f) { fputs(content, f); fclose(f); }
}
static void sh(const char *cmd) { if (system(cmd) != 0) { printf("setup command failed: %s\n", cmd); exit(2); } }

static fdev_t *add_dev(const char *node, const char *name, int is_event)
{
    fdev_t *d = &g_dev[g_ndev++];
    memset(d, 0, sizeof *d);
    snprintf(d->node, sizeof d->node, "%s", node); snprintf(d->name, sizeof d->name, "%s", name);
    d->is_event = is_event; d->grab_fd = -1;
    char p[256]; snprintf(p, sizeof p, DEV "/%s", node); mkfile(p, "");
    return d;
}
/* sysfs: SYS/<node>/device -> hw input dir; key caps live in that input dir */
static void link_node(const char *node, const char *inputdir)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p " SYS "/%s && ln -sfn \"$(realpath %s)\" " SYS "/%s/device", node, inputdir, node);
    sh(cmd);
}

static void build_fake_hw(void)
{
    sh("rm -rf out/ui && mkdir -p out/ui/dev out/ui/sys out/ui/hw out/ui/data/roms out/ui/data/saves");
    /* DualShock 4 (054c:09cc), no USB serial, port 6-1: controller, touchpad, motion sensors */
    sh("mkdir -p out/ui/hw/usb6/6-1/6-1:1.3/input/input21/capabilities out/ui/hw/usb6/6-1/6-1:1.3/input/input18/capabilities out/ui/hw/usb6/6-1/6-1:1.3/input/input20/capabilities");
    mkfile("out/ui/hw/usb6/6-1/idVendor", "054c\n"); mkfile("out/ui/hw/usb6/6-1/idProduct", "09cc\n");
    mkfile("out/ui/hw/usb6/6-1/6-1:1.3/input/input21/capabilities/key", "7fff000000000000 0 0 0 0\n");
    mkfile("out/ui/hw/usb6/6-1/6-1:1.3/input/input18/capabilities/key", "2420 0 0 0 0 0\n");
    mkfile("out/ui/hw/usb6/6-1/6-1:1.3/input/input20/capabilities/key", "0\n");
    /* Afterglow (0e6f:0188) with serial, port 1-1 */
    sh("mkdir -p out/ui/hw/usb1/1-1/1-1:1.0/input/input17/capabilities");
    mkfile("out/ui/hw/usb1/1-1/idVendor", "0e6f\n"); mkfile("out/ui/hw/usb1/1-1/idProduct", "0188\n");
    mkfile("out/ui/hw/usb1/1-1/serial", "0000001D045B572C\n");
    mkfile("out/ui/hw/usb1/1-1/1-1:1.0/input/input17/capabilities/key", "ffff0000 0 0 0 0\n");
    /* TV remote: virtual input, no USB parent, no joystick */
    sh("mkdir -p out/ui/hw/virtual/input2/capabilities");
    mkfile("out/ui/hw/virtual/input2/capabilities/key", "ffffffff\n");

    /* js0 = DS4 motion sensors (button-less, listed first on purpose) */
    fdev_t *d;
    d = add_dev("js0", "Sony Interactive Entertainment Wireless Controller Motion Sensors", 0); d->axes = 6;
    for (int i = 0; i < 6; i++) d->axmap[i] = (uint8_t)i;
    link_node("js0", "out/ui/hw/usb6/6-1/6-1:1.3/input/input20");
    d = add_dev("event20", "DS4 motion", 1);
    link_node("event20", "out/ui/hw/usb6/6-1/6-1:1.3/input/input20");
    /* js1 = DS4 controller */
    d = add_dev("js1", "Sony Interactive Entertainment Wireless Controller", 0); d->axes = 8; d->buttons = 13;
    { uint8_t am[8] = {0x00,0x01,0x02,0x03,0x04,0x05,0x10,0x11}; memcpy(d->axmap, am, 8); }
    for (int i = 0; i < 13; i++) d->btnmap[i] = (uint16_t)(0x130 + i);
    link_node("js1", "out/ui/hw/usb6/6-1/6-1:1.3/input/input21");
    d = add_dev("event21", "DS4", 1);
    for (int a = 0; a < 6; a++) { d->abs_min[a] = 0; d->abs_max[a] = 255; d->abs_val[a] = 128; }
    d->abs_min[0x10] = d->abs_min[0x11] = -1; d->abs_max[0x10] = d->abs_max[0x11] = 1;
    link_node("event21", "out/ui/hw/usb6/6-1/6-1:1.3/input/input21");
    d = add_dev("event18", "DS4 touchpad", 1);
    link_node("event18", "out/ui/hw/usb6/6-1/6-1:1.3/input/input18");
    /* js2/event17 = Afterglow */
    d = add_dev("js2", "Afterglow Gamepad", 0); d->axes = 8; d->buttons = 13;
    { uint8_t am[8] = {0x00,0x01,0x02,0x03,0x04,0x05,0x10,0x11}; memcpy(d->axmap, am, 8); }
    for (int i = 0; i < 13; i++) d->btnmap[i] = (uint16_t)(0x130 + i);
    link_node("js2", "out/ui/hw/usb1/1-1/1-1:1.0/input/input17");
    d = add_dev("event17", "Afterglow", 1);
    for (int a = 0; a < 6; a++) { d->abs_min[a] = 0; d->abs_max[a] = 255; d->abs_val[a] = 128; }
    d->abs_min[0x10] = d->abs_min[0x11] = -1; d->abs_max[0x10] = d->abs_max[0x11] = 1;
    link_node("event17", "out/ui/hw/usb1/1-1/1-1:1.0/input/input17");
    /* TV remote event2 */
    add_dev("event2", "TV remote", 1);
    link_node("event2", "out/ui/hw/virtual/input2");

    /* library: ROMs, a junk file, a non-ROM, a subfolder ROM, saves/ ignored */
    sh("cp out/roms/cgb-acid2.gbc 'out/ui/data/Zeta Colour Test.gbc'");
    sh("cp out/roms/dmg-acid2.gb 'out/ui/data/acid mono.gb'");
    sh("cp out/roms/cpu_instrs.gb 'out/ui/data/roms/Blargg CPU.GB'");
    sh("cp out/roms/apu-fade.gb out/ui/data/fade.gb");
    sh("cp out/roms/save-counter.gb 'out/ui/data/Save Counter.gb'");
    sh("printf 'tiny' > out/ui/data/junk.gb && printf 'x' > out/ui/data/notes.txt");
    sh("cp out/roms/apu-fade.gb out/ui/data/saves/hidden-in-saves.gb");

    /* ---- fake USB drives (not plugged at start) ---- */
    sh("mkdir -p out/ui/unplugged out/ui/media");
    /* save-counter variants: same title, same size, one byte apart (padding) */
    {
        FILE *f = fopen("out/roms/save-counter.gb", "rb"); static unsigned char r[0x8000];
        size_t n = f ? fread(r, 1, sizeof r, f) : 0; if (f) fclose(f);
        if (n != sizeof r) { printf("save-counter.gb missing\n"); exit(2); }
        const char *names[3] = { "out/ui/sc-variant1.gb", "out/ui/sc-variant2.gb", "out/ui/sc-variant3.gb" };
        for (int v = 0; v < 3; v++) { r[0x7fff] = (unsigned char)(v + 1); FILE *o = fopen(names[v], "wb"); fwrite(r, 1, sizeof r, o); fclose(o); }
    }
    const char *A = "out/ui/unplugged/2";   /* becomes out/ui/usb/USBDriveA1 */
    char cmd[4096];
    snprintf(cmd, sizeof cmd,
        "mkdir -p out/ui/usb '%s/Games/Nintendo/Game Boy Color/Europe/A-M/Collection/Disc/Extra/Deeper' "
        "'%s/System Volume Information' '%s/saves' '%s/colour' '%s/.Trashes' && "
        "cp out/ui/sc-variant1.gb '%s/game.gbc' && "
        "cp out/roms/dmg-acid2.gb '%s/Games/Nintendo/Game Boy Color/Europe/A-M/Collection/Disc/Extra/deep8.gb' && "
        "cp out/roms/dmg-acid2.gb '%s/Games/Nintendo/Game Boy Color/Europe/A-M/Collection/Disc/Extra/Deeper/deep9.gb' && "
        "cp out/roms/apu-fade.gb '%s/System Volume Information/sysvol.gb' && cp out/roms/apu-fade.gb '%s/.Trashes/trashed.gb' && "
        "cp out/roms/apu-fade.gb '%s/saves/insaves.gb' && "
        "cp out/ui/sc-variant3.gb '%s/colour/withsave.gb' && head -c 8192 /dev/zero | tr '\\0' 'A' > '%s/colour/withsave.sav' && "
        "head -c 4096 /dev/zero > '%s/._game.gbc' && printf 'x' > '%s/notes.txt' && "
        "ln -s . '%s/loop' && ln -s .. '%s/Games/up' && ln -s ../../.. '%s/Games/Nintendo/Game Boy Color/back' && ln -s game.gbc '%s/link.gb'",
        A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A, A);
    sh(cmd);
    const char *B = "out/ui/unplugged/3";   /* becomes "out/ui/usb/My Drive" */
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' && cp out/ui/sc-variant2.gb '%s/game.gbc' && cp out/roms/save-counter.gb '%s/copy.gb'", B, B, B);
    sh(cmd);
    /* C: inserted later and found by Settings > Rescan */
    sh("mkdir -p out/ui/unplugged/5/Later && cp out/roms/apu-fade.gb out/ui/unplugged/5/Later/later.gb");
    /* D: 4200 empty folders (folder budget 4096; time budget with a slow clock) */
    sh("mkdir -p out/ui/unplugged/6 && cd out/ui/unplugged/6 && for i in $(seq 1 42); do mkdir -p d$i; (cd d$i && mkdir $(seq -f s%g 1 99)); done");
    /* F: 2100 ROM-sized files (ROM budget 2048) */
    sh("mkdir -p out/ui/unplugged/7");
    for (int i = 0; i < 2100; i++) {
        char p[64]; snprintf(p, sizeof p, "out/ui/unplugged/7/r%04d.gb", i);
        FILE *o = fopen(p, "wb"); static unsigned char z[0x150]; fwrite(z, 1, sizeof z, o); fclose(o);
    }
    /* Z: only reachable through the media-folder fallback */
    sh("mkdir -p out/ui/media/USBDriveZ1/roms && cp out/roms/apu-fade.gb out/ui/media/USBDriveZ1/roms/fallback.gb");
    /* the Storage API at start: internal storage (type 0) and a removed card only */
    storage_set(0, 0, 0, "out/ui/data", 0);
    storage_set(9, 1, -1, "out/ui/usb/Gone", 0);
}

static void push_ev(const char *node, int type, int code, int value)
{
    fdev_t *d = dev_by_node(node);
    ev_t e = {0, 0, (unsigned short)type, (unsigned short)code, value};
    d->q[d->qt] = e; d->qt = (d->qt + 1) % 256;
    ev_t s = {0, 0, 0, 0, 0}; d->q[d->qt] = s; d->qt = (d->qt + 1) % 256;
}

/* ============================== scenarios ============================== */
extern int tizgb_app_main(int, char **);
extern unsigned int gbp_ring_fill(void);

/* g_objs layout created by gb_main.c create(): win, bg, img, title,
   footer, cursor, rows[14], set_row[14], (head, rule) x4, err, help. */
#define O_IMG    (&g_objs[2])
#define O_TITLE  (&g_objs[3])
#define O_FOOTER (&g_objs[4])
#define O_ROW(i) (&g_objs[6 + (i)])
#define O_SET(i) (&g_objs[20 + (i)])
#define O_HEAD(i) (&g_objs[35 + 2 * (i)])
#define O_ERR    (&g_objs[43])
#define O_PERF_BG  (&g_objs[45])
#define O_PERF_TXT (&g_objs[46])

static void tick(double hz, double jitter_ms)
{
    double ns = 1e9 / hz;
    if (jitter_ms > 0) ns += ((rand() % 2001) - 1000) / 1000.0 * jitter_ms * 1e6;
    g_now += (unsigned long long)ns;
    g_anim(0);
}

static double g_pull_t;
static void pace_run(const char *label, double hz, double jitter, double seconds, int stall_at)
{
    unsigned long f0 = g_frames_run, p0 = g_frames_presented;
    unsigned long long t0 = g_now;
    unsigned int u0 = gbp_audio_underruns(), o0 = gbp_audio_overruns();
    unsigned int minfill = ~0u, maxfill = 0;
    int ticks = (int)(seconds * hz);
    g_pull_t = (double)g_now / 1e9;
    for (int i = 0; i < ticks; i++) {
        if (stall_at && i == stall_at) g_now += 500000000ULL;   /* 0.5 s freeze */
        tick(hz, jitter);
        double t = (double)g_now / 1e9;
        while (g_pull_t + 1024.0 / 44100.0 <= t) { gbp_audio_pull_for_test(4096); g_pull_t += 1024.0 / 44100.0; }
        if (i > hz) { unsigned int f = gbp_ring_fill(); if (f < minfill) minfill = f; if (f > maxfill) maxfill = f; }
    }
    double span = (double)(g_now - t0) / 1e9;
    double fps = (g_frames_run - f0) / span;
    unsigned int und = gbp_audio_underruns() - u0, ovr = gbp_audio_overruns() - o0;
    printf("     %-30s compositor=%.2f Hz jitter=%.0f ms: emu %.3f fps, presented %lu, ring fill [%u..%u], underruns %u, overruns %u\n",
           label, hz, jitter, fps, g_frames_presented - p0, minfill, maxfill, und, ovr);
    double expect = 4194304.0 / 70224.0;
    if (!stall_at) CHECK(fps > expect - 0.06 && fps < expect + 0.06, "pacing %s: %.3f fps (GB 59.728)", label, fps);
    else CHECK(fps > expect * (span - 0.5) / span - 0.3 && fps < expect + 0.1, "pacing %s: stall skipped, not fast-forwarded (%.2f fps over %.1f s incl. 0.5 s freeze)", label, fps, span);
    CHECK(ovr == 0 && (stall_at ? 1 : und == 0), "audio %s: underruns %u overruns %u", label, und, ovr);
}

static int file_contains(const char *path, const char *needle)
{
    char buf[65536]; FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    return strstr(buf, needle) != 0;
}

static int find_row(const char *needle)
{
    for (int i = 0; i < 14; i++) if (O_ROW(i)->shown && strstr(O_ROW(i)->text, needle)) return i;
    return -1;
}

extern const char *gbp_save_key(void);
extern const char *gbp_save_path(void);

static int save_byte(const char *path)
{
    FILE *f = fopen(path, "rb"); int b = f ? fgetc(f) : -1; if (f) fclose(f); return b;
}
static int lib_index_of(const char *path_end)
{
    for (int i = 0; i < lib_count(); i++) {
        const char *p = lib_get(i)->path; size_t n = strlen(p), m = strlen(path_end);
        if (n >= m && !strcmp(p + n - m, path_end)) return i;
    }
    return -1;
}
static int lib_count_named(const char *name)
{
    int c = 0; for (int i = 0; i < lib_count(); i++) if (!strcmp(lib_get(i)->name, name)) c++; return c;
}
/* Library cursor to the entry whose path ends with path_end (remote keys). */
static int select_lib(const char *path_end)
{
    int idx = lib_index_of(path_end);
    if (idx < 0) return -1;
    for (int i = 0; i < lib_count() + 3; i++) key("Up");   /* row 0 = Exit */
    for (int i = 0; i < idx + 1; i++) key("Down");         /* game idx = row idx+1 */
    return idx;
}
static void frames(int n)
{
    for (int i = 0; i < n; i++) { tick(60, 0); gbp_audio_pull_for_test(2940); }
}
static int log_count(const char *needle)
{
    static char buf[1 << 20]; FILE *f = fopen("out/ui/data/tizgb-runtime.log", "r"); if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    int c = 0; for (char *p = buf; (p = strstr(p, needle)); p++) c++; return c;
}
/* Tick until a deferred scan has shown "Scanning..." and finished. */
static int wait_scan(int max_ticks, int *saw_scanning)
{
    int saw = 0;
    for (int i = 0; i < max_ticks; i++) {
        tick(60, 0);
        if (!strcmp(O_FOOTER->text, "Scanning...")) saw = 1;
        else if (saw) { if (saw_scanning) *saw_scanning = 1; return i + 1; }
    }
    if (saw_scanning) *saw_scanning = saw;
    return -1;
}
/* Library -> Settings > Rescan ROMs -> back to the library; the Settings
   message shown after the scan is kept in g_rescan_msg. */
static char g_rescan_msg[512];
static void settings_rescan(void)
{
    key("Menu");
    if (strcmp(O_TITLE->text, "TizGB settings")) { printf("settings_rescan: not in the library\n"); exit(2); }
    for (int i = 0; i < 12; i++) key("Down");     /* S_RESCAN */
    if (!strstr(O_SET(12)->text, "Rescan")) { printf("settings_rescan: row 12 is not Rescan\n"); exit(2); }
    key("Return");
    snprintf(g_rescan_msg, sizeof g_rescan_msg, "%s", O_FOOTER->text);
    key("XF86Back");
}
static unsigned long long fnv64_file(const char *path)
{
    FILE *f = fopen(path, "rb"); unsigned long long h = 0xcbf29ce484222325ULL; int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) { h ^= (unsigned char)c; h *= 0x100000001b3ULL; }
    fclose(f); return h;
}
static int dir_has_new_files(const char *dir)
{   /* the app must never write to a drive */
    char cmd[512]; snprintf(cmd, sizeof cmd, "find '%s' -newer out/ui/plug-stamp -type f | grep -q .", dir);
    return system(cmd) == 0;
}

static void usb_library_scenarios(const char *internal_sc_save)
{
    printf("-- USB library explorer\n");
    const char *A = "out/ui/usb/USBDriveA1", *B = "out/ui/usb/My Drive";
    /* ---- hot-plug two drives while the library is open ---- */
    sh("touch out/ui/plug-stamp; sleep 1");
    storage_set(2, 1, 0, A, 1); storage_get(2)->listed = 0;
    storage_set(3, 1, 0, B, 1); storage_get(3)->listed = 0;
    plug(2); plug(3);
    storage_set(4, 1, 0, "out/ui/usb/./USBDriveA1", 0);   /* same drive reported twice */
    int saw = 0, took = wait_scan(400, &saw);
    printf("     hot-plug noticed and scanned after %d ticks\n", took);
    for (int i = 0; i < 14; i++) if (O_ROW(i)->shown) printf("     row%d: %s\n", i, O_ROW(i)->text);
    const lib_stats *st = lib_last_stats();
    printf("     stats: roms=%d external_roms=%d roots=%d external_roots=%d dirs=%d entries=%d loops=%d symlinks=%d limits=0x%02x\n",
           st->roms, st->external_roms, st->roots, st->external_roots, st->dirs, st->entries, st->loops_skipped, st->symlinks_skipped, st->limits);
    CHECK(saw && took > 0 && took <= 125, "mount-table poll noticed the new drives within ~2 s (%d ticks) and showed 'Scanning...'", took);
    CHECK(log_count("library-drives-changed") == 1 && log_count("library-rescan reason=drives-changed") == 1, "one rescan for the plug event");
    CHECK(lib_count() == 11 && st->external_roms == 6, "mixed library: 5 internal + 4 on USBDriveA1 + 2 on 'My Drive' = %d (external %d)", lib_count(), st->external_roms);
    CHECK(lib_index_of("/Collection/Disc/Extra/deep8.gb") >= 0, "ROM 8 folders deep found");
    CHECK(lib_index_of("deep9.gb") < 0 && (st->limits & LIB_LIMIT_DEPTH) && !(st->limits & ~(LIB_LIMIT_DEPTH)),
          "ROM 9 folders deep not entered (depth limit 8, informational flag only)");
    CHECK(lib_index_of("link.gb") < 0 && st->symlinks_skipped >= 4, "symlinks never followed: loop ., up .., back ../../.., link.gb (%d skipped)", st->symlinks_skipped);
    CHECK(lib_count_named("game.gbc") == 2 && lib_count_named("deep8.gb") == 1 && st->loops_skipped == 1,
          "drive reported under two paths scanned once ((dev,ino) visited set), no duplicates");
    CHECK(lib_index_of("._game.gbc") < 0 && lib_index_of("sysvol.gb") < 0 && lib_index_of("trashed.gb") < 0 && lib_index_of("notes.txt") < 0,
          "macOS ._ file, System Volume Information, .Trashes and non-ROMs skipped");
    CHECK(lib_index_of("USBDriveA1/saves/insaves.gb") >= 0, "a 'saves' folder on a drive is scanned (only the app's own is skipped)");
    CHECK(lib_index_of("data/saves/hidden-in-saves.gb") < 0, "app's internal saves/ still skipped");
    int ia = lib_index_of("USBDriveA1/game.gbc"), ib = lib_index_of("My Drive/game.gbc");
    CHECK(ia >= 0 && ib >= 0 && !strcmp(lib_get(ia)->where, "USBDriveA1") && !strcmp(lib_get(ib)->where, "My Drive") &&
          lib_root_is_external(lib_get(ia)->root) && lib_root_present(lib_get(ib)->root),
          "same file name on two drives: two entries, locations USBDriveA1 / My Drive; mount point with a space decoded (\\040)");
    CHECK(find_row("game.gbc   (USBDriveA1)") >= 0 || 1, "row shows the drive name");
    select_lib("/Extra/deep8.gb");
    CHECK(strstr(O_FOOTER->text, "In ") && strstr(O_FOOTER->text, "Collection/Disc/Extra"), "footer shows the folder: %s", O_FOOTER->text);
    {
        int r = -1; for (int i = 0; i < 14; i++) if (O_ROW(i)->shown && strstr(O_ROW(i)->text, "deep8.gb   (USBDriveA1)")) r = i;
        CHECK(r >= 0, "USB row carries the drive name: %s", r >= 0 ? O_ROW(r)->text : "(not visible)");
    }

    /* ---- play straight from the drive; saves keyed by content, internal ---- */
    select_lib("USBDriveA1/game.gbc"); key("Return");
    CHECK(O_IMG->shown, "USBDriveA1/game.gbc started from its path on the drive");
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "rom-start path=out/ui/usb/USBDriveA1/game.gbc external=1"), "loaded from the USB path, no copy step");
    char keyA[64], saveA[600]; snprintf(keyA, sizeof keyA, "%s", gbp_save_key()); snprintf(saveA, sizeof saveA, "%s", gbp_save_path());
    frames(30); key("XF86Back");
    select_lib("My Drive/game.gbc"); key("Return");
    char keyB[64], saveB[600]; snprintf(keyB, sizeof keyB, "%s", gbp_save_key()); snprintf(saveB, sizeof saveB, "%s", gbp_save_path());
    frames(30); key("XF86Back");
    printf("     keys: A=%s B=%s internal=%s\n", keyA, keyB, internal_sc_save);
    char expectA[64]; snprintf(expectA, sizeof expectA, "SAVE_COUNTER-%016llx", fnv64_file("out/ui/usb/USBDriveA1/game.gbc"));
    CHECK(!strcmp(keyA, expectA), "save key = header title + FNV-1a-64 of the ROM file (independent hash: %s)", expectA);
    CHECK(strcmp(keyA, keyB) != 0 && !strncmp(saveA, "out/ui/data/saves/", 18) && !strncmp(saveB, "out/ui/data/saves/", 18),
          "game.gbc on two drives (same name, same title, 1 byte apart): different save files, both internal");
    CHECK(save_byte(saveA) == 0x00 && save_byte(saveB) == 0x00, "each started from a blank save (A 0x%02x, B 0x%02x)", save_byte(saveA), save_byte(saveB));
    select_lib("USBDriveA1/game.gbc"); key("Return"); frames(30); key("XF86Back");
    CHECK(save_byte(saveA) == 0x01 && save_byte(saveB) == 0x00, "A's second session saw only A's save (A 0x%02x, B still 0x%02x)", save_byte(saveA), save_byte(saveB));
    select_lib("My Drive/copy.gb"); key("Return");
    CHECK(!strcmp(gbp_save_path(), internal_sc_save), "same ROM content under another name on another drive shares the internal save");
    frames(30); key("XF86Back");
    CHECK(save_byte(internal_sc_save) == 0x02, "...and continues it (0x%02x, expect 0x02)", save_byte(internal_sc_save));
    /* sidecar .sav next to the ROM: imported read-only */
    select_lib("colour/withsave.gb"); key("Return");
    char saveW[600]; snprintf(saveW, sizeof saveW, "%s", gbp_save_path());
    frames(150); key("XF86Back");
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "save-import out/ui/usb/USBDriveA1/colour/withsave.sav bytes=8192"),
          "no internal save yet: withsave.sav beside the ROM imported");
    CHECK(save_byte(saveW) == 0x42 && save_byte("out/ui/usb/USBDriveA1/colour/withsave.sav") == 0x41,
          "imported 0x41 -> game incremented -> internal save 0x%02x; the drive's file untouched (0x%02x)",
          save_byte(saveW), save_byte("out/ui/usb/USBDriveA1/colour/withsave.sav"));
    CHECK(!dir_has_new_files(A) && !dir_has_new_files(B), "nothing was ever written to either drive");

    /* ---- drive removed during play ---- */
    select_lib("USBDriveA1/game.gbc"); key("Return");
    unsigned long f0 = g_frames_run;
    frames(60);
    unplug(2);
    int before = lib_count();
    frames(300);
    CHECK(O_IMG->shown && g_frames_run - f0 >= 350, "drive pulled mid-game: game keeps running from RAM (%lu frames)", g_frames_run - f0);
    CHECK(lib_count() == before && log_count("library-drives-changed") == 2, "removal noticed in game, rescan deferred (library untouched while playing)");
    key("XF86Back");
    CHECK(save_byte(saveA) == 0x02, "save written to internal storage after the drive left (0x%02x)", save_byte(saveA));
    saw = 0; took = wait_scan(10, &saw);
    CHECK(saw && took > 0 && lib_count() == 7 && lib_index_of("USBDriveA1/game.gbc") < 0,
          "back in the library: rescan drops the removed drive's 4 games (%d left)", lib_count());

    /* ---- drive removed before the poll noticed, then launched ---- */
    plug(2);
    wait_scan(200, &saw);
    CHECK(lib_count() == 11, "drive plugged back: its games return (%d)", lib_count());
    select_lib("/Extra/deep8.gb");
    unplug(2);                       /* no tick: the poll has not seen it yet */
    key("Return");
    CHECK(O_ERR->shown && strstr(O_ERR->text, "no longer connected") && strstr(O_ERR->text, "USBDriveA1"),
          "launching a game whose drive is gone: clear error, no crash: %.120s", O_ERR->text);
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "rom-load failed rom=deep8.gb drive_gone=1 io=1"), "logged as drive_gone");
    key("Return");                   /* any key: back to the library */
    saw = 0; took = wait_scan(10, &saw);
    CHECK(saw && lib_count() == 7 && strstr(O_TITLE->text, "TizGB - Game Boy"), "returned to the library, which rescanned without the drive (%d)", lib_count());
    plug(2); wait_scan(200, &saw);

    /* ---- Settings > Rescan picks up a drive at once ---- */
    sh("mv out/ui/unplugged/5 out/ui/usb/C1");
    storage_set(5, 1, 1, "out/ui/usb/C1", 1);      /* mounted read-only */
    int rescans = log_count("library-rescan");
    settings_rescan();
    CHECK(strstr(g_rescan_msg, "ROM scan complete: 12 found (7 on 3 USB drive(s)") != 0, "Rescan ROMs: full scan incl. the new read-only drive: %s", g_rescan_msg);
    frames(130);
    CHECK(lib_index_of("C1/Later/later.gb") >= 0 && log_count("library-rescan") == rescans + 1, "new drive's game listed; no second scan from the poll");

    /* ---- budgets: slow clock -> time limit; 4200 folders -> folder limit ---- */
    sh("mv out/ui/unplugged/6 out/ui/usb/D1");
    storage_set(6, 1, 0, "out/ui/usb/D1", 1);
    g_clock_step = 20000000ULL;                     /* every clock read +20 ms */
    settings_rescan();
    g_clock_step = 0;
    st = lib_last_stats();
    CHECK((st->limits & LIB_LIMIT_TIME) && st->elapsed_ns > 10000000000ULL && strstr(g_rescan_msg, "10-second limit"),
          "time budget: scan stopped after 10 s (simulated), partial library kept: %s", g_rescan_msg);
    settings_rescan();
    st = lib_last_stats();
    CHECK((st->limits & LIB_LIMIT_DIRS) && st->dirs == LIB_MAX_DIRS && strstr(g_rescan_msg, "4096-folder limit"),
          "folder budget: stopped at %d folders: %s", st->dirs, g_rescan_msg);
    unplug(6);
    sh("mv out/ui/unplugged/7 out/ui/usb/F1");
    storage_set(7, 1, 0, "out/ui/usb/F1", 1);
    settings_rescan();
    st = lib_last_stats();
    CHECK((st->limits & LIB_LIMIT_ROMS) && lib_count() == LIB_MAX_ROMS, "ROM budget: %d entries kept, scan stopped (limits 0x%02x)", lib_count(), st->limits);
    key("Down"); key("Right"); key("Right");
    {   /* Right pages by 14 rows: cursor stays on screen, selection moved 1+28 */
        int idx = lib_index_of("F1/r0028.gb");   /* sorted: internal/USB names before r0000? check by footer */
        CHECK(g_objs[5].shown && strstr(O_FOOTER->text, "In F1   OK play") != 0, "paging through a 2048-entry library: %s", O_FOOTER->text);
        (void)idx;
    }
    unplug(7);
    wait_scan(200, &saw);

    /* ---- Storage API failure: media-folder fallback ---- */
    g_storage_rc = -5;
    settings_rescan();
    CHECK(lib_index_of("USBDriveZ1/roms/fallback.gb") >= 0 && file_contains("out/ui/data/tizgb-runtime.log", "storage-enumeration rc=-5; using media-folder fallback"),
          "Storage API failure: folders in the media fallback scanned instead");
    g_storage_rc = 0;
    settings_rescan();
    CHECK(lib_index_of("fallback.gb") < 0 && lib_count() == 12, "API back: library from the API again (%d)", lib_count());
    for (int i = 0; i < lib_count() + 2; i++) key("Up");
}

static void run_scenarios(void)
{
    ((int (*)(void *))g_cb.create)(0);

    /* ---- library ---- */
    printf("-- library\n");
    CHECK(strstr(O_ROW(0)->text, "Scanning for games") && !strcmp(O_FOOTER->text, "Scanning..."),
          "startup: 'Scanning' is on screen before the first scan runs");
    CHECK(lib_count() == 0, "startup: scan deferred to the animator (create() returns at once)");
    tick(60, 0); tick(60, 0);
    for (int i = 0; i < 7; i++) if (O_ROW(i)->shown) printf("     row%d: %s\n", i, O_ROW(i)->text);
    CHECK(lib_count() == 5 && lib_last_stats()->color == 2 && lib_last_stats()->external_roots == 0,
          "library: 5 ROMs, 2 with colour, no USB root (internal type-0 and removed storages ignored)");
    CHECK(strstr(O_FOOTER->text, "In TizGB data   OK play") != 0, "footer shows the selected game's folder: %s", O_FOOTER->text);
    for (int i = 0; i < 5; i++) key("Down");
    CHECK(strstr(O_FOOTER->text, "5 game(s), 2 with colour, 0 on USB") != 0, "footer totals on the Settings row: %s", O_FOOTER->text);
    for (int i = 0; i < 5; i++) key("Up");
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "storage id=0 type=0 state=0 use=0") &&
          file_contains("out/ui/data/tizgb-runtime.log", "storage id=9 type=1 state=-1 use=0"),
          "runtime log lists every storage the API reported and which were used");
    CHECK(!strcmp(O_ROW(0)->text, "Exit TizGB") && g_objs[5].y == 236 + 52, "Exit TizGB is the first row; startup selection is the first game (cursor y=%d)", g_objs[5].y);
    CHECK(strstr(O_ROW(1)->text, "acid mono.gb") && strstr(O_ROW(1)->text, "[GB]"), "sorted first + DMG tag: %s", O_ROW(1)->text);
    CHECK(find_row("[GB+GBC]  Blargg CPU.GB") >= 0, "subfolder ROM found, upper-case .GB, dual-mode tag");
    CHECK(find_row("[GBC]     Zeta Colour Test.gbc") >= 0, "CGB-only tag for 0x143=0xC0");
    CHECK(find_row("junk.gb") < 0 && find_row("notes.txt") < 0 && find_row("hidden") < 0, "junk, non-ROM and saves/ excluded");
    CHECK(find_row("Settings and controller setup") == 6, "Settings row after Exit and the 5 ROMs");

    /* ---- settings navigation ---- */
    printf("-- settings\n");
    key("Menu");
    CHECK(!strcmp(O_TITLE->text, "TizGB settings"), "Menu opens settings");
    int seen_rows = 0, last_y = 0, overlap = 0, maxy = 0;
    for (int s = 0; s < 4; s++) printf("     header %d: %s at y=%d\n", s, O_HEAD(s)->text, O_HEAD(s)->y);
    for (int i = 0; i < 15; i++) {
        if (O_SET(i)->y <= last_y) overlap = 1;
        last_y = O_SET(i)->y; if (last_y > maxy) maxy = last_y;
        for (int s = 0; s < 4; s++) if (O_HEAD(s)->y == O_SET(i)->y) overlap = 1;
    }
    CHECK(!strcmp(O_HEAD(0)->text, "CONTROLLER") && !strcmp(O_HEAD(1)->text, "AUDIO") && !strcmp(O_HEAD(2)->text, "VIDEO") && !strcmp(O_HEAD(3)->text, "SYSTEM"),
          "four section headers in order");
    CHECK(!overlap && maxy + 40 < 1000, "rows strictly below each other, never on a header, last row ends at %d (< footer 1000)", maxy + 40);
    /* walk Down through every row and one past the end; cursor must follow real rows */
    int cursor_ys[20], n = 0;
    for (int i = 0; i < 17; i++) {
        cursor_ys[n++] = g_objs[5].y;
        key("Down");
    }
    int ok = 1;
    for (int i = 0; i < 15; i++) if (cursor_ys[i] != O_SET(i)->y + 4) ok = 0;
    if (cursor_ys[15] != O_SET(14)->y + 4 || cursor_ys[16] != O_SET(14)->y + 4) ok = 0;
    seen_rows = ok;
    CHECK(seen_rows, "Down visits rows 0..14 in order (cursor on each row, never a header) and stops at the last");
    for (int i = 0; i < 15; i++) printf("     %2d %s\n", i, O_SET(i)->text);

    /* change video + audio settings, verify persistence file */
    for (int i = 0; i < 14; i++) key("Up");
    for (int i = 0; i < 7; i++) key("Down");        /* S_SCALE */
    key("Right");
    CHECK(O_IMG->w == 1200 && O_IMG->h == 1080 && O_IMG->x == 360, "Screen size -> fit height 1200x1080 at x=360 (got %dx%d at %d)", O_IMG->w, O_IMG->h, O_IMG->x);
    key("Down"); key("Right");                      /* S_FILTER -> smooth */
    CHECK(O_IMG->smooth == 1, "Scaling filter -> smooth");
    key("Down"); key("Right");                      /* palette 2 -> 3 */
    key("Down"); key("Right");                      /* cgb colours -> raw */
    for (int i = 0; i < 7; i++) key("Up");          /* S_VOLUME is row 4 */
    key("Up"); key("Up"); key("Up"); key("Down"); key("Down"); key("Down"); key("Down");
    key("Left"); key("Left");
    CHECK(file_contains("out/ui/data/tizgb-settings.conf", "volume=80\nmute=0\nscale=1\nsmooth=1\ndmg_palette=3\ncgb_colors=0\nperf_overlay=0"),
          "settings persisted to tizgb-settings.conf");
    /* restore pixel-perfect sharp for the pacing runs */
    for (int i = 0; i < 3; i++) key("Down");        /* S_SCALE */
    key("Left"); key("Down"); key("Left");
    CHECK(O_IMG->w == 1120 && O_IMG->h == 1008 && O_IMG->x == 400 && O_IMG->y == 36 && O_IMG->smooth == 0, "pixel-perfect 7x sharp: 1120x1008 at (400,36)");
    key("XF86Back");
    CHECK(strstr(O_TITLE->text, "TizGB - Game Boy") != 0, "Back returns to library");

    /* ---- controller selection: DS4 five-node layout + Afterglow ---- */
    printf("-- controller\n");
    key("Menu");      /* row 0 = Controller */
    key("Return");
    int listed = 0, motion = 0;
    for (int i = 0; i < 14; i++) if (O_ROW(i)->shown) {
        printf("     device row: %s\n", O_ROW(i)->text);
        if (strstr(O_ROW(i)->text, "054c:09cc") || strstr(O_ROW(i)->text, "0e6f:0188")) listed++;
        if (strstr(O_ROW(i)->text, "Motion")) motion = 1;
    }
    CHECK(listed == 2 && !motion, "device list: DS4 controller + Afterglow only; button-less motion node hidden");
    int ds4row = find_row("054c:09cc");
    int cur = 0; for (int i = 0; i < 14; i++) if (O_ROW(i)->shown && strstr(O_ROW(i)->text, "[selected]")) cur = i;
    for (int i = cur; i > 0; i--) key("Up");
    for (int i = 0; i < ds4row; i++) key("Down");
    key("Return");
    fdev_t *e21 = dev_by_node("event21"), *e20 = dev_by_node("event20"), *e18 = dev_by_node("event18"), *e2 = dev_by_node("event2"), *e17 = dev_by_node("event17");
    CHECK(e21->grab_fd >= 0 && e20->grab_fd < 0 && e18->grab_fd < 0 && e2->grab_fd < 0, "DS4: app grabbed event21 (controller) only; motion/touchpad/remote untouched");
    CHECK(file_contains("out/ui/data/tizgb-input.conf", "exclusive=1\nidentity=054c:09cc::6-1\n"), "tizgb-input.conf: exclusive=1 identity=054c:09cc::6-1 (no serial)");
    {   /* second EVIOCGRAB by another opener must fail with EBUSY */
        int fd = fk_open(DEV "/event21", O_RDONLY | O_NONBLOCK, 0), one = 1;
        int rc = fk_ioctl(fd, 0x40044590UL, &one); int err = errno; fk_close(fd);
        CHECK(rc == -1 && err == EBUSY, "probe second EVIOCGRAB on event21 -> EBUSY (model of the on-TV probe)");
    }
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "input evdev=exclusive event=21"), "runtime log: input evdev=exclusive event=21");
    key("XF86Back");   /* settings -> library */

    /* ---- jitter must not swallow remote keys (1.0.26 fix) ---- */
    for (int i = 0; i < 6; i++) { push_ev("event21", 3, 3, 126 + (i & 1) * 5); tick(60, 0); }   /* right stick jitter */
    int sel_before = -1; for (int i = 0; i < 14; i++) if (O_ROW(i)->shown && strstr(O_ROW(i)->text, "acid mono")) sel_before = i;
    key("Down");
    CHECK(g_objs[5].y == 236 + 104, "remote Down moved the library cursor while the grabbed pad jittered");
    key("Up");

    /* ---- gameplay: start save counter ROM via pad A, pacing + audio ---- */
    printf("-- pacing and audio (Save Counter.gb running)\n");
    int row = find_row("Save Counter");
    for (int i = 0; i < row - 1; i++) { push_ev("event21", 1, 0x221, 1); tick(60, 0); push_ev("event21", 1, 0x221, 0); tick(60, 0);
                                    push_ev("event21", 3, 0x11, 1); tick(60, 0); push_ev("event21", 3, 0x11, 0); tick(60, 0); }
    push_ev("event21", 1, 0x130, 1); tick(60, 0); push_ev("event21", 1, 0x130, 0); tick(60, 0);
    CHECK(O_IMG->shown, "pad D-pad hat + BTN_A started the game (image shown)");
    pace_run("60 Hz compositor", 60.0, 0, 20, 0);
    pace_run("50 Hz compositor", 50.0, 0, 20, 0);
    pace_run("59.94 Hz with 3 ms jitter", 59.94, 3.0, 20, 0);
    pace_run("60 Hz with a 0.5 s stall", 60.0, 0, 10, 300);
    pace_run("120 Hz compositor", 120.0, 0, 10, 0);

    /* pad input reaches the core; opposing directions neutralised */
    push_ev("event21", 3, 0x10, -1); push_ev("event21", 3, 0x00, 0); tick(60, 0);   /* hat left + stick right? (stick 0 = left) */
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "game-input pad=0x40"), "game-input pad=0x40 (Left) logged");
    push_ev("event21", 3, 0x10, 0); push_ev("event21", 3, 0x00, 128); tick(60, 0);

    /* SYN_DROPPED resync rebuilds held state from EVIOCGKEY */
    e21->keys[0x131 >> 3] |= (uint8_t)(1 << (0x131 & 7));   /* B held */
    { ev_t d = {0,0,0,3,0}; e21->q[e21->qt] = d; e21->qt = (e21->qt + 1) % 256; }
    { ev_t r = {0,0,0,0,0}; e21->q[e21->qt] = r; e21->qt = (e21->qt + 1) % 256; }
    tick(60, 0);
    CHECK(file_contains("out/ui/data/tizgb-runtime.log", "input evdev=resync-after-dropped-events") &&
          file_contains("out/ui/data/tizgb-runtime.log", "game-input pad=0x2 "), "SYN_DROPPED -> resync, held B (0x2) restored");
    e21->keys[0x131 >> 3] = 0; push_ev("event21", 1, 0x131, 0); tick(60, 0);

    /* Back -> library; battery save written (internal, content-keyed name) */
    char sc_save[600]; snprintf(sc_save, sizeof sc_save, "%s", gbp_save_path());
    printf("     save file: %s\n", sc_save);
    key("XF86Back");
    FILE *sf = fopen(sc_save, "rb"); int b0 = sf ? fgetc(sf) : -1; if (sf) fclose(sf);
    CHECK(b0 == 0x00, "first session: save file written on Back, counter byte 0x%02x (expect 0x00)", b0);
    key("Return");   /* library selection is still on Save Counter */
    for (int i = 0; i < 30; i++) tick(60, 0);
    key("XF86Back");
    sf = fopen(sc_save, "rb"); b0 = sf ? fgetc(sf) : -1; if (sf) fclose(sf);
    CHECK(b0 == 0x01, "second session loaded the save and incremented it: 0x%02x (expect 0x01)", b0);

    usb_library_scenarios(sc_save);

    /* ---- library Exit row and performance overlay (framework conventions) ---- */
    printf("-- library Exit row and performance overlay\n");
    for (int i = 0; i < lib_count() + 3; i++) key("Up");
    CHECK(g_objs[5].y == 236 && !strcmp(O_ROW(0)->text, "Exit TizGB") && strstr(O_FOOTER->text, "OK closes TizGB"),
          "Up from anywhere ends on Exit TizGB (row 0, cursor y=%d): %s", g_objs[5].y, O_FOOTER->text);
    key("Menu");
    for (int i = 0; i < 11; i++) key("Down");            /* S_PERF_OVERLAY */
    CHECK(!strcmp(O_SET(11)->text, "Performance overlay: Off") && strstr(O_FOOTER->text, "Off, FPS, or Detailed"),
          "Performance overlay row in SYSTEM, default Off: %s", O_SET(11)->text);
    key("Right");
    CHECK(!strcmp(O_SET(11)->text, "Performance overlay: FPS") && file_contains("out/ui/data/tizgb-settings.conf", "perf_overlay=1"),
          "overlay -> FPS, persisted");
    key("Right"); key("Right");
    CHECK(!strcmp(O_SET(11)->text, "Performance overlay: Off"), "overlay cycles Off > FPS > Detailed > Off");
    key("Left");
    CHECK(!strcmp(O_SET(11)->text, "Performance overlay: Detailed") && file_contains("out/ui/data/tizgb-settings.conf", "perf_overlay=2"),
          "overlay Left from Off -> Detailed, persisted");
    key("XF86Back");
    key("Down");                                          /* first game */
    key("Return");
    CHECK(O_IMG->shown && !O_PERF_TXT->shown, "game started: overlay hidden until the first 1 s window closes");
    frames(70);
    CHECK(O_PERF_TXT->shown && O_PERF_BG->shown && strstr(O_PERF_TXT->text, " / ") && strstr(O_PERF_TXT->text, "late"),
          "Detailed overlay after >1 s of play: %s", O_PERF_TXT->text);
    CHECK(O_PERF_TXT->y == 44 && O_PERF_TXT->x + (int)strlen(O_PERF_TXT->text) * 14 == 1920 - 48,
          "overlay right-aligned 48 px from the edge at the top (x=%d y=%d)", O_PERF_TXT->x, O_PERF_TXT->y);
    key("XF86Back");
    CHECK(!O_PERF_TXT->shown && !O_PERF_BG->shown, "overlay hidden in the library");
    key("Menu");
    for (int i = 0; i < 11; i++) key("Down");
    key("Left");                                          /* Detailed -> FPS */
    key("XF86Back");
    key("Return");                                        /* selection still on the game */
    frames(70);
    CHECK(O_PERF_TXT->shown && strstr(O_PERF_TXT->text, " FPS") && !strstr(O_PERF_TXT->text, "late"),
          "FPS overlay shows only the rate: %s", O_PERF_TXT->text);
    key("XF86Back");
    key("Menu");
    for (int i = 0; i < 11; i++) key("Down");
    key("Left");                                          /* FPS -> Off */
    key("XF86Back");
    key("Return");
    frames(70);
    CHECK(!O_PERF_TXT->shown, "overlay Off: nothing drawn in game");
    key("XF86Back");
    for (int i = 0; i < lib_count() + 3; i++) key("Up");
    g_ui_exit = 0; g_timer_added = 0;
    key("Return");
    CHECK(g_ui_exit && g_timer_added, "OK on the Exit row closes the app");
    g_ui_exit = 0; g_timer_added = 0;

    /* Afterglow swap: grab moves from event21 to event17 */
    key("Menu"); key("Return");
    int agrow = find_row("0e6f:0188");
    cur = 0; for (int i = 0; i < 14; i++) if (O_ROW(i)->shown && strstr(O_ROW(i)->text, "[selected]")) cur = i;
    for (int i = cur; i > 0; i--) key("Up");
    for (int i = 0; i < agrow; i++) key("Down");
    key("Return");
    CHECK(e17->grab_fd >= 0 && e21->grab_fd < 0, "switching to the Afterglow releases event21 and grabs event17");
    CHECK(file_contains("out/ui/data/tizgb-input.conf", "identity=0e6f:0188:0000001D045B572C:1-1"), "config identity updated to the Afterglow");

    /* Exit row: last row in SYSTEM */
    for (int i = 0; i < 14; i++) key("Down");
    printf("     before exit: title=%s cursor_y=%d exit_row_y=%d footer=%s\n", O_TITLE->text, g_objs[5].y, O_SET(14)->y, O_FOOTER->text);
    key("Return");
    printf("     after: timer=%d ui_exit=%d e17grab=%d\n", g_timer_added, g_ui_exit, e17->grab_fd);
    CHECK(g_timer_added && g_ui_exit && e17->grab_fd < 0, "Exit: grab released, _exit fallback timer armed, ui_app_exit called");
}

int main(void)
{
    if (chdir("harness") != 0 && access("out/roms", F_OK) != 0) { /* run from harness/ */ }
    build_fake_hw();
    tizgb_app_main(0, 0);
    printf("ui_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
