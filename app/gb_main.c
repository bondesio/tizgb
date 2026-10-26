/*
 * gb_main.c - Tizen native UI frontend for the Walnut-CGB core (TizGB).
 *
 * Freestanding C (-nostdinc, no libc headers): every external symbol is
 * hand-declared and resolved against libraries pulled read-only from the TV.
 * Pairs with platform.c (the "gbp_*" bridge). Architecture and every input
 * fix are carried over from TizNES (nes-emulator/app/nes_main.c 1.0.26):
 *
 *  - frames are paced by CLOCK_MONOTONIC at the Game Boy's 59.7275 Hz, never
 *    one per Ecore animator tick (the TV compositor ran at 50 Hz on some
 *    boots and 60 Hz on others; TizNES 1.0.18-1.0.23 ran 20 % fast);
 *  - Settings is sectioned from day one (non-selectable headers, D-pad walks
 *    only real rows) and every setting persists;
 *  - the library (library.c) finds .gb/.gbc files in the app data folder
 *    and, up to 8 folders deep, on every drive the Tizen Storage API
 *    reports mounted; games play straight from the drive (loaded into RAM)
 *    with saves kept internally. The mount table is polled every ~2 s so
 *    inserting or removing a pendrive updates the library by itself; the
 *    CGB flag at 0x143 picks colour or DMG rendering;
 *  - one controller slot: USB identity vid:pid:serial:port, joystick nodes
 *    without key capabilities ignored (DualShock 4 motion sensors), saved pad
 *    re-resolved on every retry, exclusive EVIOCGRAB of its event node after
 *    the root watcher (tizgb-input-access.service) grants access, SYN_DROPPED
 *    resync, and grabbed-pad jitter never swallows TV-remote keys.
 *
 * UI states: LIBRARY -> GAME, plus SETTINGS, DEVICES, MAPPING, INPUT_TEST,
 * HELP and ERROR. Video is an Evas image fed the 160x144 ARGB buffer.
 */

typedef unsigned char  Eina_Bool;
typedef unsigned int   u32;
typedef struct { void *create, *terminate, *pause, *resume, *app_control; } callbacks_t;
typedef void Evas_Object;
typedef void Evas;

/* ---- appfw / EFL (hand-declared) ---- */
extern int   ui_app_main(int, char **, const callbacks_t *, void *);
extern Evas_Object *elm_win_util_standard_add(const char *, const char *);
extern void  elm_win_fullscreen_set(Evas_Object *, int);
extern void  elm_win_resize_object_add(Evas_Object *, Evas_Object *);
extern void  elm_object_focus_set(Evas_Object *, Eina_Bool);
extern Evas *evas_object_evas_get(Evas_Object *);
extern Evas_Object *evas_object_rectangle_add(Evas *);
extern Evas_Object *evas_object_text_add(Evas *);
extern Evas_Object *evas_object_image_filled_add(Evas *);
extern Evas_Object *elm_label_add(Evas_Object *);
extern void  elm_object_part_text_set(Evas_Object *, const char *, const char *);
extern void  evas_object_image_size_set(Evas_Object *, int, int);
extern void  evas_object_image_data_set(Evas_Object *, void *);
extern void  evas_object_image_data_update_add(Evas_Object *, int, int, int, int);
extern void  evas_object_image_smooth_scale_set(Evas_Object *, Eina_Bool);
extern void  evas_object_image_alpha_set(Evas_Object *, Eina_Bool);
extern void  evas_object_text_font_set(Evas_Object *, const char *, int);
extern void  evas_object_text_text_set(Evas_Object *, const char *);
extern void  evas_object_color_set(Evas_Object *, int, int, int, int);
extern void  evas_object_move(Evas_Object *, int, int);
extern void  evas_object_resize(Evas_Object *, int, int);
extern void  evas_object_show(Evas_Object *);
extern void  evas_object_hide(Evas_Object *);

extern int   evas_object_text_horiz_advance_get(const Evas_Object *);

extern int   ECORE_EVENT_KEY_DOWN;
extern int   ECORE_EVENT_KEY_UP;
extern void *ecore_event_handler_add(int, Eina_Bool (*)(void *, int, void *), const void *);
extern void *ecore_animator_add(Eina_Bool (*)(void *), const void *);
extern void *ecore_timer_add(double, Eina_Bool (*)(void *), const void *);
extern void  ui_app_exit(void);
extern void  _exit(int);

typedef struct { const char *keyname; const char *key; const char *string; } Ecore_Event_Key;

/* ---- libc / syscalls (hand-declared; libc.so.6 linked) ---- */
extern int   open(const char *, int, ...);
extern int   access(const char *, int);
#define F_OK 0
#define R_OK 4
#define W_OK 2
extern long  read(int, void *, unsigned long);
extern long  readlink(const char *, char *, unsigned long);
extern char *realpath(const char *, char *);
extern long  write(int, const void *, unsigned long);
extern int   close(int);
extern long  syscall(long, ...);
extern int   ioctl(int, unsigned long, ...);
extern int  *__errno_location(void);
extern int   snprintf(char *, unsigned long, const char *, ...);
extern unsigned long strlen(const char *);
extern int   strcasecmp(const char *, const char *);
extern int   strncmp(const char *, const char *, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
struct gb_timespec { long tv_sec; long tv_nsec; };   /* glibc layout (long, long) */
extern int   clock_gettime(int, struct gb_timespec *);
#define CLOCK_MONOTONIC 1

/* ---- bridge to platform.c ---- */
extern unsigned int *gbp_framebuffer(void);
extern int  gbp_width(void);
extern int  gbp_height(void);
extern void gbp_init(void);
extern int  gbp_load_rom(const char *, const char *);
extern int  gbp_run_frame(void);
extern void gbp_set_pad(unsigned int);
extern void gbp_stop(void);
extern int  gbp_save_now(void);
extern int  gbp_is_cgb(void);
extern const char *gbp_last_error(void);
extern void gbp_audio_set_mute(int);
extern void gbp_audio_set_volume(int);
extern int  gbp_audio_get_volume(void);
extern int  gbp_audio_is_muted(void);
extern void gbp_audio_set_channel_filter(int);
extern void gbp_set_frame_period_ns(unsigned int);
extern void gbp_set_dmg_palette(int);
extern int  gbp_dmg_palette_count(void);
extern const char *gbp_dmg_palette_name(int);
extern void gbp_set_cgb_colors(int);
extern int  gbp_last_error_is_io(void);
extern const char *gbp_save_key(void);
extern void gbp_diag(const char *);

#include "library.h"
#include "perf_overlay.h"

#ifndef TIZGB_DATA_DIR
#define TIZGB_DATA_DIR "/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data"
#endif
#ifndef TIZGB_DEV_INPUT
#define TIZGB_DEV_INPUT "/dev/input"
#endif
#ifndef TIZGB_SYS_INPUT
#define TIZGB_SYS_INPUT "/sys/class/input"
#endif

#define O_RDONLY     0
#define O_WRONLY     1
#define O_NONBLOCK   0x800
#define O_DIRECTORY  0x4000
#define O_CREAT      0x40
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define SYS_getdents64 217        /* ARM EABI */
#define JSIOCGAXES     0x80016a11UL
#define JSIOCGBUTTONS  0x80016a12UL
#define JSIOCGNAME(len) (0x80006a13UL | ((unsigned long)(len) << 16))
#define JSIOCGAXMAP    0x80406a32UL
#define JSIOCGBTNMAP   0x84006a34UL
#define ABS_X          0x00
#define ABS_Y          0x01
#define ABS_RX         0x03
#define ABS_RY         0x04
#define ABS_HAT0X      0x10
#define ABS_HAT0Y      0x11
#define BTN_A          0x130
#define BTN_B          0x131
#define BTN_SELECT     0x13a
#define BTN_START      0x13b
#define BTN_MODE       0x13c
#define EAGAIN         11
#define EINTR          4
#define EVIOCGRAB      0x40044590UL
#define EVIOCGABS(axis) (0x80184540UL | (unsigned long)(axis))
#define EVIOCGKEY(len) (0x80004518UL | ((unsigned long)(len) << 16))
#define EV_SYN         0x00
#define SYN_REPORT     0
#define SYN_DROPPED    3
#define EV_KEY         0x01
#define EV_ABS         0x03
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#define EV_KEY_CODE_COUNT 768
#define ABS_CODE_COUNT 64

typedef struct { int value, minimum, maximum, fuzz, flat, resolution; } input_absinfo;

/* Pad bits (same layout as TizNES; platform.c converts for the core) */
#define PAD_A      0x01
#define PAD_B      0x02
#define PAD_SELECT 0x04
#define PAD_START  0x08
#define PAD_UP     0x10
#define PAD_DOWN   0x20
#define PAD_LEFT   0x40
#define PAD_RIGHT  0x80

/* Opposing directions from several sources in one frame: neutral, not the
   last one wins (TizNES 1.0.21; games would otherwise see Up+Down). */
static unsigned int resolve_opposing_directions(unsigned int bits)
{
    if ((bits & (PAD_UP | PAD_DOWN)) == (PAD_UP | PAD_DOWN)) bits &= ~(PAD_UP | PAD_DOWN);
    if ((bits & (PAD_LEFT | PAD_RIGHT)) == (PAD_LEFT | PAD_RIGHT)) bits &= ~(PAD_LEFT | PAD_RIGHT);
    return bits;
}

#define TV_W 1920
#define TV_H 1080
#define PATH_MAX 512
#define VIS_ROWS 14

/* Settings rows in on-screen (and D-pad) order; section headers are not
   rows and can never be selected. */
enum {
    S_DEVICE, S_EXCLUSIVE, S_MAP, S_INPUT_TEST,
    S_VOLUME, S_MUTE, S_AUDIO_TEST,
    S_SCALE, S_FILTER, S_DMG_PALETTE, S_CGB_COLORS,
    S_PERF_OVERLAY, S_RESCAN, S_BACK, S_EXIT,
    SETTINGS_ROWS
};
typedef struct { int first_row; const char *title; } settings_section;
static const settings_section g_settings_sections[] = {
    { S_DEVICE,  "CONTROLLER" },
    { S_VOLUME,  "AUDIO" },
    { S_SCALE,   "VIDEO" },
    { S_PERF_OVERLAY, "SYSTEM" },
};
#define SETTINGS_SECTIONS ((int)(sizeof g_settings_sections / sizeof g_settings_sections[0]))

enum { ST_LIBRARY, ST_GAME, ST_HELP, ST_ERROR, ST_SETTINGS, ST_MAPPING, ST_DEVICES, ST_INPUT_TEST };

static Evas_Object *g_win, *g_bg, *g_img;
static Evas        *g_canvas;
static Evas_Object *g_title, *g_footer, *g_err;
static Evas_Object *g_row[VIS_ROWS], *g_cursor;
static Evas_Object *g_set_row[SETTINGS_ROWS];
static Evas_Object *g_set_head[SETTINGS_SECTIONS], *g_set_rule[SETTINGS_SECTIONS];
static Evas_Object *g_help;
static void        *g_anim;

static int   g_state = ST_LIBRARY;
static int   g_help_return_state = ST_GAME;
#define g_romcount lib_count()   /* library.c owns the list */
static int   g_sel, g_top;
/* Library scans run from the animator so the screen can say "Scanning"
   first: phase 1 = show the message, 2 = scan on the next tick. */
static int   g_scan_phase;
static int   g_rescan_pending;           /* drives changed while not in the library */
static char  g_library_message[160];     /* footer note until the next key */
static unsigned long g_mount_poll_frame;
#define MOUNT_POLL_FRAMES 120            /* ~2 s of animator ticks */

static unsigned int g_keybits;   /* pad bits from TV-remote/keyboard keys */

/* The controller. pad_t keeps TizNES's per-slot layout so the architecture
   can grow a second slot later (the root watcher already has two). */
typedef struct {
    int slot;
    int js_fd, ev_fd;
    unsigned int jsbits;
    unsigned int button_bits, stick_bits, hat_bits, ev_button_bits, mapped_axis_bits;
    unsigned char axis_count, button_count;
    unsigned char axis_map[64];
    unsigned short button_map[512];
    int axis_map_ok, button_map_ok;
    unsigned char button_pad[64], custom_mapped[64];
    unsigned char ev_key_pad[EV_KEY_CODE_COUNT];
    unsigned char axis_neg_pad[64], axis_pos_pad[64];
    signed char axis_state[64];
    int ev_abs_min[ABS_CODE_COUNT], ev_abs_max[ABS_CODE_COUNT];
    unsigned char ev_abs_range_ok[ABS_CODE_COUNT];
    int x_axis, y_axis, hat_x_axis, hat_y_axis;
    unsigned char axis_logged[64], button_logged[64];
    int device_index, ev_device_index, selected_js_index;
    char device_name[128];
    int capture_exclusive;
    char selected_usb_id[160];
    char map_model[16];
    int map_loaded;
    unsigned long capture_retry_frame, open_retry_frame;
    char capture_status[128];
} pad_t;
static pad_t g_pad1;
#define PAD1 (&g_pad1)
static unsigned int g_js_prevbits;
static int g_exclusive_preferred = 1;
static int g_device_indices[16], g_device_count, g_device_sel;
static char g_device_names[16][128];
static char g_device_usb_ids[16][160];
static unsigned long g_input_frame, g_js_activity_frame;
static int g_game_input_log_bits = -1;
static int g_js_activity_valid;
static int g_settings_sel, g_map_step = -1, g_map_armed;
static char g_map_detected[96];
static int g_input_test_step = -1, g_input_test_wait_release;
static unsigned short g_input_test_wait_code, g_input_test_wait_type;
static char g_input_test_last[160];
static char g_settings_message[160];

/* Persistent settings (tizgb-settings.conf, key=value). */
static int g_volume = 100, g_muted;
static int g_audio_test_filter;          /* diagnostic, not persisted */
static int g_scale_mode;                 /* 0 integer 7x, 1 fit height, 2 stretch 4:3 */
static int g_smooth;                     /* 0 sharp (nearest), 1 smooth */
static int g_dmg_palette = 2;            /* auto GBC-style colours by title */
static int g_cgb_colors = 1;             /* 1 LCD-corrected, 0 raw */
static int g_perf_mode;                  /* perf_overlay_mode: Off / FPS / Detailed */
static perf_stats g_perf;
static Evas_Object *g_perf_bg, *g_perf_txt;
static const char *g_scale_names[3] = {
    "Pixel-perfect 7x (1120 x 1008)", "Fit screen height (1200 x 1080)", "Stretch 4:3 (1440 x 1080)" };
static const char *g_audio_test_names[5] = {
    "All channels", "No Pulse 1", "No Pulse 2", "No Wave", "No Noise" };

/* Pacing: one Game Boy frame is 70224 cycles of 4194304 Hz = 16.742706 ms
   (59.7275 Hz). Paced by the monotonic clock, independent of the animator
   tick (compositor) rate. */
#define PACE_GB_NS 16742706u
#define PACE_MAX_PER_TICK 3      /* catch-up frames per animator tick */
#define PACE_MAX_LAG 6           /* beyond this many late frames, drop the backlog */
static unsigned int g_pace_period_ns = PACE_GB_NS;
static unsigned long long g_pace_t0, g_pace_done;
static unsigned long long g_pace_win_t0, g_pace_win_frames;
static unsigned long g_pace_ticks, g_pace_idle, g_pace_multi, g_pace_stalls;

static const char *g_settings_path     = TIZGB_DATA_DIR "/tizgb-settings.conf";
static const char *g_input_config_path = TIZGB_DATA_DIR "/tizgb-input.conf";
static const char *g_input_test_log_path = TIZGB_DATA_DIR "/tizgb-gamepad-test.log";
static const char *g_map_dir = TIZGB_DATA_DIR;

static const char *g_input_test_labels[] = {
    "A", "B", "X", "Y", "LB", "RB", "LT / ZL", "RT / ZR",
    "Select", "Start", "Left stick click", "Right stick click", "Home / Guide",
    "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left"
};
#define INPUT_TEST_COUNT ((int)(sizeof(g_input_test_labels)/sizeof(g_input_test_labels[0])))

/* Per-model mapping files tizgb-map-<vid>-<pid>.bin; same block layout as
   TizNES's TNS1 files (bytes 132-134 unused), magic TGB1. */
#define MAP_FILE_SIZE (263 + EV_KEY_CODE_COUNT)

static void settings_open(void);
static void settings_close(void);
static void settings_activate(void);
static void settings_adjust(int direction);
static void app_exit(void);
static void start_game(int idx);
static void device_select_open(void);
static void device_select_activate(void);
static void help_open(int return_state);
static void help_close(void);
static void js_close_device(pad_t *p);
static int  identity_matches(const char *saved, const char *current);
static void apply_video_settings(void);

/* ---------- small string helpers ---------- */
static void str_copy(char *d, const char *s, int max)
{
    int i = 0; while (s[i] && i < max-1) { d[i]=s[i]; i++; } d[i]=0;
}
/* js_open_device retries every ~0.5 s while the controller is unavailable;
   log once on any change, then a heartbeat with a repeat count. */
#define DIAG_RL_SLOTS 3   /* 0 evdev line, 1 js line, 2 open-failed */
static char g_diag_last[DIAG_RL_SLOTS][192];
static unsigned int g_diag_repeat[DIAG_RL_SLOTS];
static void diag_rl(int slot, const char *line)
{
    if (slot<0 || slot>=DIAG_RL_SLOTS) { gbp_diag(line); return; }
    if (strncmp(line, g_diag_last[slot], sizeof g_diag_last[slot]) == 0) {
        g_diag_repeat[slot]++;
        if (g_diag_repeat[slot] % 20) return;
        char line2[224];
        snprintf(line2, sizeof line2, "%s (repeated x%u)", line, g_diag_repeat[slot]);
        gbp_diag(line2);
        return;
    }
    str_copy(g_diag_last[slot], line, sizeof g_diag_last[slot]);
    g_diag_repeat[slot] = 1;
    gbp_diag(line);
}

/* ---------- ROM library (scanning lives in library.c) ---------- */
static void render_library(void);

/* Full rescan, keeping the cursor on the same game when it is still there. */
static void library_rescan(const char *reason)
{
    char keep[LIB_PATH_MAX]; keep[0] = 0;
    const lib_rom *cur = lib_get(g_sel - 1);
    if (cur) str_copy(keep, cur->path, sizeof keep);
    int before = lib_count();
    int on_settings = before > 0 && g_sel > before;
    int first_scan = strncmp(reason, "startup", 8) == 0;
    char line[96]; snprintf(line, sizeof line, "library-rescan reason=%s", reason);
    gbp_diag(line);
    lib_scan();
    g_rescan_pending = 0;
    int at = lib_find_path(keep);
    if (at >= 0) g_sel = at + 1;
    else if (on_settings || g_sel > lib_count() + 1) g_sel = lib_count() + 1;
    else if (first_scan) g_sel = lib_count() > 0 ? 1 : 0;   /* start on a game, not Exit */
    const lib_stats *st = lib_last_stats();
    g_library_message[0] = 0;
    if (st->limits & (LIB_LIMIT_ROMS | LIB_LIMIT_DIRS | LIB_LIMIT_ENTRIES | LIB_LIMIT_TIME))
        snprintf(g_library_message, sizeof g_library_message,
                 "Library incomplete: scan stopped at the %s limit (%d games listed)",
                 (st->limits & LIB_LIMIT_ROMS) ? "2048-game" : (st->limits & LIB_LIMIT_TIME) ? "10-second" :
                 (st->limits & LIB_LIMIT_DIRS) ? "4096-folder" : "200000-file", lib_count());
    else if (strncmp(reason, "startup", 8) != 0 && strncmp(reason, "settings", 9) != 0)
        snprintf(g_library_message, sizeof g_library_message,
                 "Library updated: %d game(s) (%+d), %d on %d USB drive(s)",
                 lib_count(), lib_count() - before, st->external_roms, st->external_roots);
}

/* Ask for a scan; it runs from the library screen (deferred elsewhere). */
static char g_scan_reason[24];
static void library_request_scan(const char *reason)
{
    g_rescan_pending = 1;
    str_copy(g_scan_reason, reason, sizeof g_scan_reason);
}

/* Animator hook, every tick: poll the mount table (a change there only
   triggers a scan if the Storage API's set of mounted drives changed too),
   then run a pending scan once the library is on screen. */
static void library_tick(unsigned long frame)
{
    if (frame - g_mount_poll_frame >= MOUNT_POLL_FRAMES) {
        g_mount_poll_frame = frame;
        if (lib_mounts_changed() && lib_roots_changed()) {
            gbp_diag("library-drives-changed");
            library_request_scan("drives-changed");
        }
    }
    if (g_state != ST_LIBRARY) return;
    if (g_scan_phase == 2) {
        g_scan_phase = 0;
        library_rescan(g_scan_reason);
        render_library();
    } else if (g_scan_phase == 1) {
        g_scan_phase = 2;
        render_library();
    } else if (g_rescan_pending) {
        g_scan_phase = 1;
        render_library();
    }
}

/* ---------- settings persistence ---------- */
static int parse_int(const char *s) { int v = 0, neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0'); return neg ? -v : v; }

static void load_settings(void)
{
    char data[512]; int fd = open(g_settings_path, O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, data, sizeof data - 1); close(fd);
        if (n > 0) {
            data[n] = 0;
            char *p = data;
            while (*p) {
                char *end = p; while (*end && *end != '\n') end++;
                if (*end) *end++ = 0;
                if (!strncmp(p, "volume=", 7)) g_volume = parse_int(p+7);
                else if (!strncmp(p, "mute=", 5)) g_muted = parse_int(p+5) ? 1 : 0;
                else if (!strncmp(p, "scale=", 6)) g_scale_mode = parse_int(p+6);
                else if (!strncmp(p, "smooth=", 7)) g_smooth = parse_int(p+7) ? 1 : 0;
                else if (!strncmp(p, "dmg_palette=", 12)) g_dmg_palette = parse_int(p+12);
                else if (!strncmp(p, "cgb_colors=", 11)) g_cgb_colors = parse_int(p+11) ? 1 : 0;
                else if (!strncmp(p, "perf_overlay=", 13)) g_perf_mode = parse_int(p+13);
                p = end;
            }
        }
    }
    if (g_volume < 0) g_volume = 0;
    if (g_volume > 100) g_volume = 100;
    if (g_scale_mode < 0 || g_scale_mode > 2) g_scale_mode = 0;
    g_perf_mode = perf_mode_sanitize(g_perf_mode);
    if (g_dmg_palette < 0 || g_dmg_palette >= gbp_dmg_palette_count()) g_dmg_palette = 2;
    gbp_audio_set_volume(g_volume); gbp_audio_set_mute(g_muted);
    gbp_set_dmg_palette(g_dmg_palette); gbp_set_cgb_colors(g_cgb_colors);
}

static void save_settings(void)
{
    char data[256];
    int n = snprintf(data, sizeof data, "volume=%d\nmute=%d\nscale=%d\nsmooth=%d\ndmg_palette=%d\ncgb_colors=%d\nperf_overlay=%d\n",
                     g_volume, g_muted, g_scale_mode, g_smooth, g_dmg_palette, g_cgb_colors, g_perf_mode);
    int fd = open(g_settings_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { if (write(fd, data, (unsigned long)n) != n) gbp_diag("settings-save=short-write"); close(fd); }
    else gbp_diag("settings-save=open-failed");
}

/* ---------- per-model mapping ---------- */
static void mapping_unpack(pad_t *p, const unsigned char *data, long n)
{
    memset(p->button_pad, 0, sizeof p->button_pad);
    memset(p->custom_mapped, 0, sizeof p->custom_mapped);
    memset(p->ev_key_pad, 0, sizeof p->ev_key_pad);
    memset(p->axis_neg_pad, 0, sizeof p->axis_neg_pad);
    memset(p->axis_pos_pad, 0, sizeof p->axis_pos_pad);
    if (n < MAP_FILE_SIZE || data[0]!='T' || data[1]!='G' || data[2]!='B' || data[3]!='1') return;
    memcpy(p->button_pad, data+4, 64);
    memcpy(p->custom_mapped, data+68, 64);
    memcpy(p->axis_neg_pad, data+135, 64);
    memcpy(p->axis_pos_pad, data+199, 64);
    memcpy(p->ev_key_pad, data+263, EV_KEY_CODE_COUNT);
}

static void mapping_pack(const pad_t *p, unsigned char *data)
{
    memset(data, 0, MAP_FILE_SIZE);
    data[0]='T'; data[1]='G'; data[2]='B'; data[3]='1';
    memcpy(data+4, p->button_pad, 64);
    memcpy(data+68, p->custom_mapped, 64);
    memcpy(data+135, p->axis_neg_pad, 64);
    memcpy(data+199, p->axis_pos_pad, 64);
    memcpy(data+263, p->ev_key_pad, EV_KEY_CODE_COUNT);
}

static int is_hex4(const char *s)
{
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0;
    }
    return 1;
}

/* "vvvv:pppp:serial:port" -> "vvvv-pppp"; 0 when there is no USB identity. */
static int identity_model(const char *identity, char *model, int cap)
{
    model[0] = 0;
    if (cap < 10 || strlen(identity) < 9 || identity[4] != ':' || !is_hex4(identity) || !is_hex4(identity+5)) return 0;
    for (int i = 0; i < 4; i++) { model[i] = identity[i]; model[5+i] = identity[5+i]; }
    model[4] = '-'; model[9] = 0;
    return 1;
}

static void model_map_path(const char *model, char *path, int cap)
{
    snprintf(path, (unsigned long)cap, "%s/tizgb-map-%s.bin", g_map_dir, model);
}

static void pad_save_model_mapping(pad_t *p)
{
    char path[PATH_MAX], line[PATH_MAX + 48];
    unsigned char data[MAP_FILE_SIZE];
    if (!p->map_model[0]) return;
    mapping_pack(p, data);
    model_map_path(p->map_model, path, sizeof path);
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, data, sizeof data) != sizeof data) gbp_diag("mapping-save=short-write");
        close(fd);
    } else gbp_diag("mapping-save=open-failed");
    snprintf(line, sizeof line, "mapping-save model=%s", p->map_model);
    gbp_diag(line);
}

/* No file for the model: the standard defaults (BTN_A/B/SELECT/START, left
   stick, D-pad hat) apply. */
static void pad_apply_mapping(pad_t *p)
{
    char model[16], path[PATH_MAX], line[PATH_MAX + 64];
    unsigned char data[MAP_FILE_SIZE];
    if (!identity_model(p->selected_usb_id, model, sizeof model)) {
        if (p->map_loaded && !p->map_model[0]) return;
        mapping_unpack(p, data, 0);
        p->map_model[0] = 0; p->map_loaded = 1;
        return;
    }
    if (p->map_loaded && strncmp(model, p->map_model, sizeof model) == 0) return;
    str_copy(p->map_model, model, sizeof p->map_model);
    p->map_loaded = 1;
    model_map_path(model, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, data, sizeof data); close(fd);
        mapping_unpack(p, data, n);
        snprintf(line, sizeof line, "mapping model=%s source=model-file", model);
        gbp_diag(line);
        return;
    }
    mapping_unpack(p, data, 0);
    snprintf(line, sizeof line, "mapping model=%s source=defaults", model);
    gbp_diag(line);
}

/* ---------------------------- UI helpers --------------------------------- */
static Evas_Object *mk_text(int size, int r, int g, int b)
{
    Evas_Object *t = evas_object_text_add(g_canvas);
    evas_object_text_font_set(t, "Sans", size);
    evas_object_color_set(t, r, g, b, 255);
    return t;
}

static const char *rom_tag(int idx)
{
    const lib_rom *r = lib_get(idx);
    unsigned char f = r ? r->cgb : 0;
    if ((f & 0xc0) == 0xc0) return "[GBC]   ";
    if (f & 0x80) return "[GB+GBC]";
    return "[GB]    ";
}

/* Location label for a row: the most specific end of "where". */
static const char *where_tail(const char *where, int max)
{
    int n = (int)strlen(where);
    if (n <= max) return where;
    const char *p = where + n - (max - 2);
    while (*p && *p != '/') p++;               /* restart at a folder boundary */
    return *p ? p + 1 : where + n - (max - 2);
}

static void render_library(void)
{
    char line[240];
    int total = g_romcount + 2;               /* Exit, games, Settings */
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= total) g_sel = total - 1;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + VIS_ROWS) g_top = g_sel - VIS_ROWS + 1;
    if (g_top > total - VIS_ROWS) g_top = total - VIS_ROWS > 0 ? total - VIS_ROWS : 0;
    evas_object_text_text_set(g_title, "TizGB - Game Boy and Game Boy Color");
    int scanning = g_scan_phase != 0;
    int shift = (g_romcount == 0 || scanning) ? 2 : 0;
    if (scanning) g_top = 0;
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = g_top + i - shift;
        if ((g_romcount == 0 || scanning) && i < 2) {
            const char *message = scanning
                ? (i == 0 ? "Scanning for games in the TizGB folder and on USB drives..." : "")
                : (i == 0 ? "No .gb or .gbc files found. Copy ROMs you own into the TizGB data folder,"
                          : "or insert a USB drive with ROMs; the library updates by itself.");
            evas_object_text_text_set(g_row[i], message);
            evas_object_move(g_row[i], 360, 285 + i*52);
            evas_object_color_set(g_row[i], 180, 180, 180, 255);
            evas_object_show(g_row[i]);
        } else if (idx == 0) {
            int sel = g_sel == 0;
            evas_object_text_text_set(g_row[i], "Exit TizGB");
            evas_object_move(g_row[i], 360, 230 + i*52);
            evas_object_color_set(g_row[i], sel?255:170, sel?255:170, sel?255:170, 255);
            evas_object_show(g_row[i]);
        } else if (idx >= 1 && idx <= g_romcount) {
            int game = idx - 1;
            const lib_rom *r = lib_get(game);
            char drive[20]; int k = 0;           /* first component of "where" = drive name */
            while (r->where[k] && r->where[k] != '/' && k < (int)sizeof drive - 1) { drive[k] = r->where[k]; k++; }
            drive[k] = 0;
            if (lib_root_is_external(r->root)) snprintf(line, sizeof line, "%s  %.60s   (%s)", rom_tag(game), r->name, drive);
            else snprintf(line, sizeof line, "%s  %.70s", rom_tag(game), r->name);
            int sel = idx == g_sel;
            evas_object_text_text_set(g_row[i], line);
            evas_object_move(g_row[i], 360, 230 + i*52);
            evas_object_color_set(g_row[i], sel?255:170, sel?255:170, sel?255:170, 255);
            evas_object_show(g_row[i]);
        } else if (idx == g_romcount + 1) {
            int sel = g_sel == g_romcount + 1;
            evas_object_text_text_set(g_row[i], "Settings and controller setup");
            evas_object_move(g_row[i], 360, 230 + i*52);
            evas_object_color_set(g_row[i], sel?255:170, sel?255:170, sel?255:170, 255);
            evas_object_show(g_row[i]);
        } else evas_object_hide(g_row[i]);
    }
    {
        int cursor_row = g_sel - g_top + shift;
        evas_object_move(g_cursor, 330, 236 + cursor_row*52);
        evas_object_resize(g_cursor, 8, 40);
        evas_object_show(g_cursor);
    }
    /* Footer: the selected game's folder, or the totals on the Settings row. */
    const lib_stats *st = lib_last_stats();
    const lib_rom *selr = lib_get(g_sel - 1);
    if (scanning) snprintf(line, sizeof line, "Scanning...");
    else if (g_library_message[0]) snprintf(line, sizeof line, "%s", g_library_message);
    else if (g_sel == 0) snprintf(line, sizeof line, "OK closes TizGB   %d game(s)   Start: Settings   Select: Help", g_romcount);
    else if (selr) snprintf(line, sizeof line, "In %s   OK play   Left/Right page   Start: Settings   Select: Help",
                            where_tail(selr->where, 34));
    else snprintf(line, sizeof line, "%d game(s), %d with colour, %d on USB   Left/Right page   Start: Settings   Select: Help",
                  g_romcount, st->color, st->external_roms);
    evas_object_text_text_set(g_footer, line);
}

/* OK / A on the highlighted library row: Exit first, games, Settings last.
   While the first scan message is up, only Exit responds. */
static void library_activate(void)
{
    if (g_sel == 0) app_exit();
    else if (g_scan_phase) return;
    else if (g_sel == g_romcount + 1) settings_open();
    else start_game(g_sel - 1);
}

/* Library cursor movement; Left/Right (and Channel Up/Down) page. */
static void library_move(int delta)
{
    if (g_scan_phase) return;
    g_library_message[0] = 0;
    g_sel += delta;
    if (g_sel < 0) g_sel = 0;
    if (g_sel > g_romcount + 1) g_sel = g_romcount + 1;
    render_library();
}

static int pad_present(const pad_t *p) { return p->js_fd >= 0; }

static void pad_status_text(const pad_t *p, char *out, int cap)
{
    if (!pad_present(p)) {
        snprintf(out, (unsigned long)cap, "%s", p->selected_usb_id[0] ? "selected controller not connected" : "none detected");
        return;
    }
    snprintf(out, (unsigned long)cap, "%.70s [%s]", p->device_name[0] ? p->device_name : "USB gamepad",
             p->capture_exclusive ? "exclusive" : "shared");
}

/* Layout: title at y=120, footer at y=1000; 4 headers and 14 rows fit
   between them (last row ends near y=965), as in TizNES. */
#define SET_TOP        194
#define SET_HEAD_H      44
#define SET_SECTION_GAP 14
#define SET_ROW_H       37

static void settings_text(int id, char *out, int cap)
{
    char status[160];
    switch (id) {
    case S_DEVICE: pad_status_text(PAD1, status, sizeof status); snprintf(out, (unsigned long)cap, "Controller: %s", status); break;
    case S_EXCLUSIVE: snprintf(out, (unsigned long)cap, "Exclusive capture: %s", g_exclusive_preferred ? "On" : "Off"); break;
    case S_MAP: snprintf(out, (unsigned long)cap, "Map controller buttons (D-pad, A, B, Select, Start)"); break;
    case S_INPUT_TEST: snprintf(out, (unsigned long)cap, "Guided gamepad input test (logs raw buttons)"); break;
    case S_VOLUME: snprintf(out, (unsigned long)cap, "Volume: %d%%", g_volume); break;
    case S_MUTE: snprintf(out, (unsigned long)cap, "Mute: %s", g_muted ? "On" : "Off"); break;
    case S_AUDIO_TEST: snprintf(out, (unsigned long)cap, "Audio channel test: %s", g_audio_test_names[g_audio_test_filter]); break;
    case S_SCALE: snprintf(out, (unsigned long)cap, "Screen size: %s", g_scale_names[g_scale_mode]); break;
    case S_FILTER: snprintf(out, (unsigned long)cap, "Scaling filter: %s", g_smooth ? "Smooth (bilinear)" : "Sharp pixels (nearest)"); break;
    case S_DMG_PALETTE: snprintf(out, (unsigned long)cap, "Original Game Boy palette: %s", gbp_dmg_palette_name(g_dmg_palette)); break;
    case S_CGB_COLORS: snprintf(out, (unsigned long)cap, "Game Boy Color colours: %s", g_cgb_colors ? "GBC screen (corrected)" : "Vivid (raw)"); break;
    case S_PERF_OVERLAY: snprintf(out, (unsigned long)cap, "Performance overlay: %s", perf_mode_label((perf_overlay_mode)g_perf_mode)); break;
    case S_RESCAN: snprintf(out, (unsigned long)cap, "Rescan ROMs now (TizGB folder and every USB drive)"); break;
    case S_BACK: snprintf(out, (unsigned long)cap, "Back to library"); break;
    case S_EXIT: snprintf(out, (unsigned long)cap, "Exit TizGB (close the emulator completely)"); break;
    default: out[0] = 0;
    }
}

static void render_settings(void)
{
    char text[200];
    evas_object_text_text_set(g_title, "TizGB settings");
    int y = SET_TOP, section = 0, cursor_y = SET_TOP;
    for (int id = 0; id < SETTINGS_ROWS; id++) {
        if (section < SETTINGS_SECTIONS && g_settings_sections[section].first_row == id) {
            if (section) y += SET_SECTION_GAP;
            evas_object_text_text_set(g_set_head[section], g_settings_sections[section].title);
            evas_object_move(g_set_head[section], 360, y);
            evas_object_show(g_set_head[section]);
            evas_object_move(g_set_rule[section], 360, y + 34);
            evas_object_resize(g_set_rule[section], 1200, 2);
            evas_object_show(g_set_rule[section]);
            y += SET_HEAD_H;
            section++;
        }
        int sel = id == g_settings_sel;
        settings_text(id, text, sizeof text);
        evas_object_text_text_set(g_set_row[id], text);
        evas_object_move(g_set_row[id], 390, y);
        evas_object_color_set(g_set_row[id], sel?255:180, sel?255:180, sel?255:180, 255);
        evas_object_show(g_set_row[id]);
        if (sel) cursor_y = y;
        y += SET_ROW_H;
    }
    evas_object_move(g_cursor, 364, cursor_y + 4); evas_object_resize(g_cursor, 8, 30); evas_object_show(g_cursor);

    const char *hint;
    switch (g_settings_sel) {
    case S_DEVICE: hint = "OK chooses the USB controller; its buttons are saved per controller model"; break;
    case S_EXCLUSIVE: hint = g_exclusive_preferred ? (PAD1->capture_exclusive ? "Controller held by TizGB; Tizen shortcuts are blocked"
                                                                             : "Exclusive capture: On (waiting for device access)")
                                                  : "Exclusive capture: Off; controller buttons may reach Tizen"; break;
    case S_MAP: hint = "Follow the prompts; Back cancels. Saved for this controller model"; break;
    case S_AUDIO_TEST: hint = "Left/Right picks a channel to omit, for comparing sounds in game"; break;
    case S_SCALE: case S_FILTER: hint = "Left/Right changes; applies immediately"; break;
    case S_DMG_PALETTE: hint = "Colours for original (monochrome) Game Boy games; Left/Right changes"; break;
    case S_CGB_COLORS: hint = "Corrected imitates the real GBC screen; Vivid shows raw game colours"; break;
    case S_PERF_OVERLAY: hint = "Shows emulation speed over the game: Off, FPS, or Detailed. Left/Right changes; applies immediately"; break;
    case S_EXIT: hint = "Press A / OK to quit TizGB and release the controller"; break;
    default: hint = g_exclusive_preferred ? (PAD1->capture_exclusive ? "Exclusive capture: On" : "Exclusive capture: On (waiting for device access)")
                                          : "Exclusive capture: Off; controller buttons may reach Tizen";
    }
    evas_object_text_text_set(g_footer, g_settings_message[0] ? g_settings_message : hint);
}

static void render_devices(void)
{
    char line[192];
    pad_t *p = PAD1;
    evas_object_text_text_set(g_title, "Select controller");
    if (!g_device_count) {
        evas_object_text_text_set(g_row[0], "No USB gamepads found");
        evas_object_move(g_row[0], 360, 300); evas_object_show(g_row[0]);
        evas_object_text_text_set(g_footer, "Connect a controller, then Back and reopen this screen");
        return;
    }
    for (int i = 0; i < g_device_count && i < VIS_ROWS; i++) {
        int mine = identity_matches(p->selected_usb_id, g_device_usb_ids[i]);
        snprintf(line, sizeof line, "%.80s  %.48s%s", g_device_names[i], g_device_usb_ids[i], mine ? "  [selected]" : "");
        evas_object_text_text_set(g_row[i], line);
        evas_object_move(g_row[i], 360, 220 + i*52);
        evas_object_color_set(g_row[i], i==g_device_sel?255:180, i==g_device_sel?255:180, i==g_device_sel?255:180, 255);
        evas_object_show(g_row[i]);
    }
    evas_object_move(g_cursor, 330, 226 + g_device_sel*52); evas_object_resize(g_cursor, 8, 40); evas_object_show(g_cursor);
    snprintf(line, sizeof line, "OK selects   Back returns   Capture: %s",
        p->capture_exclusive ? "exclusive" : (pad_present(p) ? p->capture_status : "not connected"));
    evas_object_text_text_set(g_footer, line);
}

static void render_mapping(void)
{
    static const char *labels[8] = {"Up", "Down", "Left", "Right", "A", "B", "Select", "Start"};
    char line[192];
    evas_object_text_text_set(g_title, "Gamepad mapping");
    for (int i = 0; i < 8; i++) {
        if (i == g_map_step) snprintf(line, sizeof line, "> %s: %s", labels[i], !g_map_armed ? "release previous input" :
            (i < 4 ? "move stick/D-pad or press a button" : "press a button"));
        else snprintf(line, sizeof line, "%s: %s", labels[i], i < g_map_step ? "mapped" : "waiting");
        evas_object_text_text_set(g_row[i], line);
        evas_object_move(g_row[i], 360, 270 + i*62);
        evas_object_show(g_row[i]);
    }
    snprintf(line, sizeof line, "Detected: %s", g_map_detected[0] ? g_map_detected : "waiting for input");
    evas_object_text_text_set(g_row[8], line);
    evas_object_move(g_row[8], 360, 780);
    evas_object_show(g_row[8]);
    evas_object_text_text_set(g_footer, PAD1->capture_exclusive ? "Exclusive capture active   Back cancels mapping" :
        "Raw event mapping needs exclusive capture; shared input may trigger Tizen   Back cancels");
}

static void render_input_test(void)
{
    char line[192];
    evas_object_text_text_set(g_title, "Gamepad input diagnostics");
    if (g_input_test_step < INPUT_TEST_COUNT) {
        snprintf(line, sizeof line, "Button %d of %d: press %s", g_input_test_step+1, INPUT_TEST_COUNT,
                 g_input_test_labels[g_input_test_step]);
        evas_object_text_text_set(g_row[0], line);
        evas_object_move(g_row[0], 360, 275); evas_object_color_set(g_row[0], 255, 255, 255, 255); evas_object_show(g_row[0]);
        snprintf(line, sizeof line, "%s", g_input_test_wait_release ? "Detected; release that control to continue" :
                 "Press only this control once; OK marks it not detected");
        evas_object_text_text_set(g_row[1], line); evas_object_move(g_row[1], 360, 350); evas_object_show(g_row[1]);
    } else {
        evas_object_text_text_set(g_row[0], "Diagnostic complete");
        evas_object_move(g_row[0], 360, 275); evas_object_color_set(g_row[0], 255, 255, 255, 255); evas_object_show(g_row[0]);
        evas_object_text_text_set(g_row[1], "Log saved in TizGB app data");
        evas_object_move(g_row[1], 360, 350); evas_object_show(g_row[1]);
    }
    snprintf(line, sizeof line, "Last event: %.150s", g_input_test_last[0] ? g_input_test_last : "none");
    evas_object_text_text_set(g_row[2], line); evas_object_move(g_row[2], 360, 455); evas_object_show(g_row[2]);
    evas_object_text_text_set(g_row[3], "For D-pad, press each direction once. For sticks, click L3/R3.");
    evas_object_move(g_row[3], 360, 535); evas_object_show(g_row[3]);
    evas_object_text_text_set(g_row[4], "OK skips an unreported control; Back cancels and saves the log.");
    evas_object_move(g_row[4], 360, 600); evas_object_show(g_row[4]);
    evas_object_text_text_set(g_footer, g_input_test_step >= INPUT_TEST_COUNT ? "OK or Back returns to Settings" :
        (g_input_test_wait_release ? "Release control to continue   Back cancels" : "Press the named gamepad control   OK = not detected   Back = cancel"));
}

static unsigned long long mono_ns(void);

/* Performance overlay (emulator-framework/docs/overlay.md): top-right, drawn
   only while a game is on screen, text rebuilt once per published window. */
static void perf_overlay_apply(void)
{
    char text[200];
    if (g_perf_mode == PERF_OVERLAY_OFF || g_state != ST_GAME ||
        perf_format(&g_perf, (perf_overlay_mode)g_perf_mode, 5973, 0, text, sizeof text) <= 0) {
        evas_object_hide(g_perf_bg); evas_object_hide(g_perf_txt);
        return;
    }
    evas_object_text_text_set(g_perf_txt, text);
    int w = evas_object_text_horiz_advance_get(g_perf_txt);
    int x = TV_W - 48 - w;
    evas_object_move(g_perf_bg, x - 10, 38); evas_object_resize(g_perf_bg, w + 20, 48);
    evas_object_move(g_perf_txt, x, 44);
    evas_object_show(g_perf_bg); evas_object_show(g_perf_txt);
}

static void show_group(int state)
{
    evas_object_hide(g_img); evas_object_hide(g_title); evas_object_hide(g_footer);
    evas_object_hide(g_err); evas_object_hide(g_help); evas_object_hide(g_cursor);
    evas_object_hide(g_perf_bg); evas_object_hide(g_perf_txt);
    for (int i = 0; i < VIS_ROWS; i++) evas_object_hide(g_row[i]);
    for (int i = 0; i < SETTINGS_ROWS; i++) evas_object_hide(g_set_row[i]);
    for (int i = 0; i < SETTINGS_SECTIONS; i++) { evas_object_hide(g_set_head[i]); evas_object_hide(g_set_rule[i]); }

    if (state == ST_LIBRARY) {
        evas_object_show(g_title); evas_object_show(g_footer);
        render_library();
    } else if (state == ST_GAME) {
        evas_object_show(g_img);
        perf_reset(&g_perf, mono_ns());
        perf_overlay_apply();
    } else if (state == ST_ERROR) {
        evas_object_show(g_err);
    } else if (state == ST_HELP) {
        if (g_help_return_state == ST_GAME) evas_object_show(g_img);
        else {
            evas_object_show(g_title); evas_object_show(g_footer);
            evas_object_text_text_set(g_title, "TizGB Help");
            evas_object_text_text_set(g_footer, "A, B, Start, Select, or Back returns");
        }
        evas_object_show(g_help);
    } else if (state == ST_SETTINGS) {
        evas_object_show(g_title); evas_object_show(g_footer); render_settings();
    } else if (state == ST_MAPPING) {
        evas_object_show(g_title); evas_object_show(g_footer); render_mapping();
    } else if (state == ST_DEVICES) {
        evas_object_show(g_title); evas_object_show(g_footer); render_devices();
    } else if (state == ST_INPUT_TEST) {
        evas_object_show(g_title); evas_object_show(g_footer); render_input_test();
    }
}

/* Screen size/filter. 160x144 is 10:9 with square pixels; 7x fills
   1120x1008 with every Game Boy pixel exactly 7x7 panel pixels. */
static void apply_video_settings(void)
{
    int w = 1120, h = 1008;
    if (g_scale_mode == 1) { w = 1200; h = 1080; }
    else if (g_scale_mode == 2) { w = 1440; h = 1080; }
    evas_object_move(g_img, (TV_W - w) / 2, (TV_H - h) / 2);
    evas_object_resize(g_img, w, h);
    evas_object_image_smooth_scale_set(g_img, g_smooth ? 1 : 0);
}

/* ---------------------------- game control -------------------------------- */
static Eina_Bool animate(void *data);

static unsigned long long mono_ns(void)
{
    struct gb_timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static void show_error(const char *headline, const char *detail)
{
    char text[640];
    snprintf(text, sizeof text,
        "<align=center><font_size=40><color=#ffb478>%s<br/>%.200s<br/><br/>"
        "Press any key to return to the library.</color></font_size></align>", headline, detail);
    elm_object_part_text_set(g_err, 0, text);
    g_state = ST_ERROR; show_group(g_state);
}

/* Back to the library from a game, an error or Settings; a scan that was
   requested meanwhile (drive inserted/removed, ROM missing) starts now. */
static void return_to_library(void)
{
    g_keybits = 0;
    g_state = ST_LIBRARY;
    if (g_rescan_pending && !g_scan_phase) g_scan_phase = 1;
    show_group(g_state);
}

static void start_game(int idx)
{
    const lib_rom *rom = lib_get(idx);
    if (!rom || g_scan_phase) return;
    g_pace_period_ns = PACE_GB_NS;
    gbp_set_frame_period_ns(g_pace_period_ns);
    g_pace_t0 = g_pace_done = 0;
    g_pace_ticks = g_pace_idle = g_pace_multi = g_pace_stalls = 0;
    {
        char line[240];
        snprintf(line, sizeof line, "pace start rom=%.120s cgb_flag=0x%02x period_ns=%u",
                 rom->name, rom->cgb, g_pace_period_ns);
        gbp_diag(line);
    }
    /* Loaded straight from its path (internal or USB) into RAM. */
    char line2[LIB_PATH_MAX + 200];
    snprintf(line2, sizeof line2, "rom-start path=%.400s external=%d", rom->path, lib_root_is_external(rom->root));
    gbp_diag(line2);
    if (gbp_load_rom(rom->path, rom->name) != 0) {
        int gone = lib_root_is_external(rom->root) && !lib_root_present(rom->root);
        snprintf(line2, sizeof line2, "rom-load failed rom=%.120s drive_gone=%d io=%d reason=%s",
                 rom->name, gone, gbp_last_error_is_io(), gbp_last_error());
        gbp_diag(line2);
        if (gone) {
            char detail[200];
            snprintf(detail, sizeof detail, "Reconnect the drive (%.60s); the library updates by itself.", lib_root_path(rom->root));
            show_error("The USB drive with this game is no longer connected.", detail);
        } else show_error("Could not load this ROM.", gbp_last_error());
        if (gone || gbp_last_error_is_io()) library_request_scan("rom-missing");
        return;
    }
    g_keybits = g_js_prevbits = 0;
    PAD1->jsbits = PAD1->button_bits = PAD1->stick_bits = PAD1->hat_bits = 0;
    PAD1->mapped_axis_bits = PAD1->ev_button_bits = 0;
    for (int i = 0; i < 64; i++) PAD1->axis_state[i] = 0;
    g_game_input_log_bits = -1;
    memset(gbp_framebuffer(), 0, (unsigned long)(gbp_width() * gbp_height() * 4));
    g_state = ST_GAME; show_group(g_state);
    if (!g_anim) g_anim = ecore_animator_add(animate, 0);
}

static void stop_game(void)
{
    gbp_stop();
    g_keybits = 0;
    PAD1->jsbits = PAD1->button_bits = PAD1->stick_bits = PAD1->hat_bits = 0;
    PAD1->ev_button_bits = 0;
    gbp_set_pad(0);
    return_to_library();
}

/* --------------------------- raw gamepad backend -------------------------- */
typedef struct { u32 time; short value; unsigned char type; unsigned char number; } js_event;
/* struct input_event: struct timeval (two longs) + type, code, value. */
typedef struct { long sec, usec; unsigned short type, code; int value; } input_event_t;
#define JS_DEAD 8000
#define PAD_RETRY_FRAMES 30   /* ~0.5 s between reopen attempts */

static void input_test_log(const char *line, int truncate_file)
{
    int flags = O_WRONLY|O_CREAT|(truncate_file ? O_TRUNC : O_APPEND);
    int fd = open(g_input_test_log_path, flags, 0644);
    if (fd < 0) { gbp_diag("input-test log=open-failed"); return; }
    unsigned long n = strlen(line);
    if (write(fd, line, n) != (long)n || write(fd, "\n", 1) != 1) gbp_diag("input-test log=short-write");
    close(fd);
}

static unsigned int input_test_aliases(unsigned short code, char *indices, int cap)
{
    pad_t *p = PAD1;
    int used = 0, first = 1;
    unsigned int bits = code < EV_KEY_CODE_COUNT ? p->ev_key_pad[code] : 0;
    if (cap > 0) indices[0] = 0;
    for (unsigned int i = 0; i < p->button_count && i < 64; i++) if (p->button_map[i] == code) {
        int n = snprintf(indices+used, (unsigned long)(cap-used), "%s%u", first ? "" : ",", i);
        if (n > 0 && n < cap-used) used += n;
        first = 0;
        bits |= p->button_pad[i];
    }
    if (first) snprintf(indices, (unsigned long)cap, "none");
    return bits;
}

static const char *pad_bit_names(unsigned int bits, char *out, int cap)
{
    int n = 0;
    out[0] = 0;
#define ADD_BIT_NAME(bit,name) if (bits&(bit)) { n += snprintf(out+n, (unsigned long)(cap-n), "%s%s", n ? "|" : "", name); }
    ADD_BIT_NAME(PAD_A,"A"); ADD_BIT_NAME(PAD_B,"B");
    ADD_BIT_NAME(PAD_SELECT,"Select"); ADD_BIT_NAME(PAD_START,"Start");
    ADD_BIT_NAME(PAD_UP,"Up"); ADD_BIT_NAME(PAD_DOWN,"Down");
    ADD_BIT_NAME(PAD_LEFT,"Left"); ADD_BIT_NAME(PAD_RIGHT,"Right");
#undef ADD_BIT_NAME
    if (!n) snprintf(out, (unsigned long)cap, "unmapped");
    return out;
}

static int input_test_expected_key(unsigned short code)
{
    if (g_input_test_step >= 0 && g_input_test_step < 13) return 1;
    if (g_input_test_step == 13) return code == BTN_DPAD_UP;
    if (g_input_test_step == 14) return code == BTN_DPAD_RIGHT;
    if (g_input_test_step == 15) return code == BTN_DPAD_DOWN;
    if (g_input_test_step == 16) return code == BTN_DPAD_LEFT;
    return 0;
}

static int input_test_axis_matches(unsigned short code, int value)
{
    if (!value) return 0;
    if (g_input_test_step == 6 || g_input_test_step == 7)
        return code != ABS_X && code != ABS_Y && code != ABS_RX && code != ABS_RY &&
               code != ABS_HAT0X && code != ABS_HAT0Y;
    if (g_input_test_step == 13) return code == ABS_HAT0Y && value < 0;
    if (g_input_test_step == 14) return code == ABS_HAT0X && value > 0;
    if (g_input_test_step == 15) return code == ABS_HAT0Y && value > 0;
    if (g_input_test_step == 16) return code == ABS_HAT0X && value < 0;
    return 0;
}

static void input_test_finish(int cancelled)
{
    char line[96];
    snprintf(line, sizeof line, "session_end status=%s", cancelled ? "cancelled" : "complete");
    input_test_log(line, 0);
    g_input_test_wait_release = 0;
    g_input_test_step = -1;
    g_state = ST_SETTINGS;
    str_copy(g_settings_message, cancelled ? "Input test cancelled; log saved" : "Input test complete; log saved", sizeof g_settings_message);
    show_group(g_state);
}

static void input_test_advance(void)
{
    g_input_test_wait_release = 0;
    g_input_test_step++;
    if (g_input_test_step >= INPUT_TEST_COUNT) { input_test_finish(0); return; }
    show_group(ST_INPUT_TEST);
}

static void input_test_skip(void)
{
    if (g_input_test_step < 0 || g_input_test_step >= INPUT_TEST_COUNT) {
        g_state = ST_SETTINGS; show_group(g_state);
        return;
    }
    char line[192];
    snprintf(line, sizeof line, "prompt=%s result=not_detected source=remote_skip", g_input_test_labels[g_input_test_step]);
    input_test_log(line, 0);
    snprintf(g_input_test_last, sizeof g_input_test_last, "%s: not detected", g_input_test_labels[g_input_test_step]);
    input_test_advance();
}

static void input_test_event(unsigned short type, unsigned short code, int value, int index, const char *source)
{
    if (g_state != ST_INPUT_TEST || g_input_test_step < 0 || g_input_test_step >= INPUT_TEST_COUNT) return;
    if (g_input_test_wait_release) {
        if (type != g_input_test_wait_type || code != g_input_test_wait_code) return;
        if (value == 0) {
            char indices[96], actions[96], line[320];
            unsigned int bits = type == EV_KEY ? input_test_aliases(code, indices, sizeof indices) : 0;
            if (type == EV_ABS) { snprintf(indices, sizeof indices, "axis"); snprintf(actions, sizeof actions, "axis"); }
            else pad_bit_names(bits, actions, sizeof actions);
            snprintf(line, sizeof line, "prompt=%s phase=release source=%s type=%u code=0x%x value=%d index=%d js_indices=%s mapped=%s",
                     g_input_test_labels[g_input_test_step], source, type, code, value, index, indices, actions);
            input_test_log(line, 0);
            input_test_advance();
        }
        return;
    }
    if (type == EV_KEY && value == 1 && input_test_expected_key(code)) {
        char indices[96], actions[96], line[320];
        unsigned int bits = input_test_aliases(code, indices, sizeof indices);
        pad_bit_names(bits, actions, sizeof actions);
        snprintf(line, sizeof line, "prompt=%s phase=press source=%s type=%u code=0x%x value=%d index=%d js_indices=%s mapped=%s",
                 g_input_test_labels[g_input_test_step], source, type, code, value, index, indices, actions);
        input_test_log(line, 0);
        snprintf(g_input_test_last, sizeof g_input_test_last, "%s code=0x%x indices=%s map=%s",
                 g_input_test_labels[g_input_test_step], code, indices, actions);
        g_input_test_wait_type = type; g_input_test_wait_code = code; g_input_test_wait_release = 1;
        show_group(ST_INPUT_TEST);
    } else if (type == EV_ABS && input_test_axis_matches(code, value)) {
        char line[256];
        snprintf(line, sizeof line, "prompt=%s phase=press source=%s type=%u code=0x%x value=%d index=%d js_indices=axis mapped=axis",
                 g_input_test_labels[g_input_test_step], source, type, code, value, index);
        input_test_log(line, 0);
        snprintf(g_input_test_last, sizeof g_input_test_last, "%s axis code=0x%x value=%d", g_input_test_labels[g_input_test_step], code, value);
        g_input_test_wait_type = type; g_input_test_wait_code = code; g_input_test_wait_release = 1;
        show_group(ST_INPUT_TEST);
    }
}

static void input_test_start(void)
{
    char line[320];
    pad_t *p = PAD1;
    g_input_test_step = 0; g_input_test_wait_release = 0; g_input_test_last[0] = 0;
    snprintf(line, sizeof line, "session_start identity=%s js=%d event=%d buttons=%u axes=%u",
             p->selected_usb_id[0] ? p->selected_usb_id : "unknown", p->device_index, p->ev_device_index,
             p->button_count, p->axis_count);
    input_test_log(line, 1);
    p->button_bits = p->stick_bits = p->hat_bits = p->mapped_axis_bits = 0;
    g_js_prevbits = 0;
    p->ev_button_bits = 0;
    g_state = ST_INPUT_TEST; show_group(g_state);
}

static unsigned int js_button_to_pad(unsigned short code)
{
    if (code == BTN_A) return PAD_A;
    if (code == BTN_B) return PAD_B;
    if (code == BTN_SELECT) return PAD_SELECT;
    if (code == BTN_START) return PAD_START;
    return 0;
}

static void js_close_device(pad_t *p)
{
    if (p->ev_fd >= 0) {
        int release = 0;
        ioctl(p->ev_fd, EVIOCGRAB, &release);
        close(p->ev_fd); p->ev_fd = -1;
    }
    if (p->js_fd >= 0) { close(p->js_fd); p->js_fd = -1; }
    p->capture_exclusive = 0;
    p->ev_device_index = -1;
    p->ev_button_bits = 0;
    p->mapped_axis_bits = 0;
    for (int i = 0; i < 64; i++) p->axis_state[i] = 0;
    for (int i = 0; i < ABS_CODE_COUNT; i++) p->ev_abs_range_ok[i] = 0;
}

static int js_read_name(int index, char *name, int cap)
{
    char path[96];
    snprintf(path, sizeof path, TIZGB_DEV_INPUT "/js%d", index);
    int fd = open(path, O_RDONLY|O_NONBLOCK);
    if (fd < 0) return 0;
    name[0] = 0;
    int ok = ioctl(fd, JSIOCGNAME(cap), name) == 0;
    name[cap-1] = 0;
    close(fd);
    return ok && name[0];
}

/* jsN and its matching eventN point to the same input-device parent in
   sysfs; compare those links so capture targets exactly the selected pad. */
static int sysfs_input_parent(const char *handler, int index, char *target, int cap)
{
    char path[160];
    snprintf(path, sizeof path, TIZGB_SYS_INPUT "/%s%d/device", handler, index);
    long n = readlink(path, target, (unsigned long)(cap-1));
    if (n <= 0 || n >= cap) return 0;
    target[n] = 0;
    return 1;
}

static int read_small_file(const char *path, char *out, int cap)
{
    int fd = open(path, O_RDONLY|O_NONBLOCK); if (fd < 0) return 0;
    long n = read(fd, out, (unsigned long)(cap-1)); close(fd);
    if (n <= 0 || n >= cap) return 0;
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ')) n--;
    out[n] = 0; return n > 0;
}

/* One pad can expose several joystick nodes under one USB identity: the
   DualShock 4 adds a button-less "Motion Sensors" joystick and has no USB
   serial. Only a node whose input device reports key capabilities is a
   controller. The root watcher applies the same rule (TizNES 1.0.26). */
static int js_has_buttons(int index)
{
    char path[160], caps[512];
    snprintf(path, sizeof path, TIZGB_SYS_INPUT "/js%d/device/capabilities/key", index);
    if (!read_small_file(path, caps, sizeof caps)) return 0;
    for (const char *c = caps; *c; c++)
        if ((*c>='1'&&*c<='9')||(*c>='a'&&*c<='f')||(*c>='A'&&*c<='F')) return 1;
    return 0;
}

static int usb_identity_for_js(int index, char *id, int cap)
{
    char link[160], resolved[512], work[512], vendor[32], product[32], serial[96];
    snprintf(link, sizeof link, TIZGB_SYS_INPUT "/js%d/device", index);
    if (!realpath(link, resolved)) return 0;
    str_copy(work, resolved, sizeof work);
    for (int depth = 0; depth < 12; depth++) {
        char path[600];
        snprintf(path, sizeof path, "%s/idVendor", work);
        if (read_small_file(path, vendor, sizeof vendor)) {
            snprintf(path, sizeof path, "%s/idProduct", work);
            if (!read_small_file(path, product, sizeof product)) return 0;
            snprintf(path, sizeof path, "%s/serial", work);
            if (!read_small_file(path, serial, sizeof serial)) serial[0] = 0;
            char port[64]; char *slash = work + strlen(work);
            while (slash > work && slash[-1] != '/') slash--;
            if (slash <= work) return 0;
            str_copy(port, slash, sizeof port);
            slash[-1] = 0;
            snprintf(id, (unsigned long)cap, "%s:%s:%s:%s", vendor, product, serial, port);
            return 1;
        }
        char *slash = work + strlen(work);
        while (slash > work && slash[-1] != '/') slash--;
        if (slash <= work) break;
        slash[-1] = 0;
    }
    return 0;
}

/* "vid:pid:serial:port": exact equality preferred; a pad with a USB serial
   is also recognized on another port (same rule as the root watcher). */
static int identity_split(const char *id, char f[4][96])
{
    int k = 0, j = 0;
    for (int i = 0; i < 4; i++) f[i][0] = 0;
    for (const char *c = id; *c; c++) {
        if (*c == ':') { f[k][j] = 0; if (++k > 3) return 0; j = 0; continue; }
        if (j < 95) f[k][j++] = *c;
    }
    f[k][j] = 0;
    return k == 3;
}
static int identity_loose_match(const char *a, const char *b)
{
    char fa[4][96], fb[4][96];
    if (!identity_split(a, fa) || !identity_split(b, fb) || !fa[2][0]) return 0;
    return strncmp(fa[0], fb[0], 96) == 0 && strncmp(fa[1], fb[1], 96) == 0 && strncmp(fa[2], fb[2], 96) == 0;
}
static int identity_matches(const char *saved, const char *current)
{
    if (!saved[0] || !current[0]) return 0;
    return strncmp(saved, current, 160) == 0 || identity_loose_match(saved, current);
}

/* jsN for the saved identity, or -1. Re-resolved on every retry, so a pad
   plugged in after launch or re-enumerated after a replug is found. */
static int pad_resolve_index(const pad_t *p)
{
    if (!p->selected_usb_id[0]) return p->selected_js_index;
    int loose = -1, loose_count = 0;
    for (int i = 0; i < 16; i++) {
        char identity[160];
        if (!usb_identity_for_js(i, identity, sizeof identity) || !js_has_buttons(i)) continue;
        if (strncmp(identity, p->selected_usb_id, sizeof identity) == 0) return i;
        if (identity_loose_match(p->selected_usb_id, identity)) { loose = i; loose_count++; }
    }
    return loose_count == 1 ? loose : -1;
}

/* tizgb-input.conf is read by the root watcher: exclusive=, identity=. */
static void save_input_config(void)
{
    char data[320];
    int n = snprintf(data, sizeof data, "exclusive=%d\nidentity=%s\n", g_exclusive_preferred ? 1 : 0, PAD1->selected_usb_id);
    if (n <= 0 || n >= (int)sizeof data) return;
    int fd = open(g_input_config_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { write(fd, data, (unsigned long)n); close(fd); }
}

static void load_input_config(void)
{
    char data[480]; int fd = open(g_input_config_path, O_RDONLY);
    if (fd < 0) return;
    long n = read(fd, data, sizeof data - 1); close(fd); if (n <= 0) return; data[n] = 0;
    char *p = data;
    while (*p) {
        char *end = p; while (*end && *end != '\n') end++;
        if (*end) *end++ = 0;
        if (strncmp(p, "exclusive=0", 11) == 0) g_exclusive_preferred = 0;
        else if (strncmp(p, "exclusive=1", 11) == 0) g_exclusive_preferred = 1;
        else if (strncmp(p, "identity=", 9) == 0) str_copy(PAD1->selected_usb_id, p+9, sizeof PAD1->selected_usb_id);
        p = end;
    }
}

/* USB controllers that can be chosen; button-less joystick nodes are not
   listed. */
static void scan_js_devices(void)
{
    pad_t *p = PAD1;
    g_device_count = 0; g_device_sel = 0;
    for (int i = 0; i < 16 && g_device_count < VIS_ROWS; i++) {
        char name[128], identity[160];
        if (usb_identity_for_js(i, identity, sizeof identity) && js_has_buttons(i)) {
            if (!js_read_name(i, name, sizeof name)) snprintf(name, sizeof name, "USB gamepad js%d", i);
            int row = g_device_count;
            g_device_indices[row] = i;
            str_copy(g_device_names[row], name, sizeof g_device_names[row]);
            str_copy(g_device_usb_ids[row], identity, sizeof g_device_usb_ids[row]);
            if (p->selected_usb_id[0] && identity_matches(p->selected_usb_id, identity)) g_device_sel = row;
            else if (!p->selected_usb_id[0] && i == p->selected_js_index) g_device_sel = row;
            g_device_count++;
        }
    }
    if (g_device_count && g_device_sel >= g_device_count) g_device_sel = 0;
}

static int target_user_mapped(const pad_t *p, unsigned int bit)
{
    for (int c = 0; c < EV_KEY_CODE_COUNT; c++) if (p->ev_key_pad[c] & bit) return 1;
    for (int i = 0; i < 64; i++) {
        if ((p->axis_neg_pad[i] | p->axis_pos_pad[i]) & bit) return 1;
        if (p->custom_mapped[i] && (p->button_pad[i] & bit)) return 1;
    }
    return 0;
}

static void js_open_device(pad_t *p, int requested_index)
{
    int wanted = requested_index;
    if (wanted < 0) wanted = p->selected_js_index;
    js_close_device(p);
    p->device_index = -1; p->device_name[0] = 0;
    if (wanted >= 0 && wanted < 16) {
        char path[96];
        snprintf(path, sizeof path, TIZGB_DEV_INPUT "/js%d", wanted);
        p->js_fd = open(path, O_RDONLY|O_NONBLOCK);
        if (p->js_fd >= 0) {
            p->device_index = wanted;
            ioctl(p->js_fd, JSIOCGNAME(sizeof p->device_name), p->device_name);
            p->device_name[sizeof p->device_name-1] = 0;
        }
    }
    /* Only when nothing was ever selected: take the first real controller
       (skipping button-less nodes), else a Tizen "Virtual Gamepad". */
    if (p->js_fd < 0 && requested_index < 0 && p->selected_js_index < 0 && !p->selected_usb_id[0]) {
        int fallback_fd = -1, fallback_index = -1;
        char fallback_name[128]; fallback_name[0] = 0;
        for (int i = 0; i < 16; i++) {
            char path[96], name[128];
            snprintf(path, sizeof path, TIZGB_DEV_INPUT "/js%d", i);
            int fd = open(path, O_RDONLY|O_NONBLOCK); if (fd < 0) continue;
            name[0] = 0; ioctl(fd, JSIOCGNAME(sizeof name), name); name[sizeof name-1] = 0;
            if (strncmp(name, "Virtual Gamepad", 15) == 0) {
                if (fallback_fd < 0) { fallback_fd = fd; fallback_index = i; str_copy(fallback_name, name, sizeof fallback_name); }
                else close(fd);
            } else if (!js_has_buttons(i)) {
                close(fd);
            } else {
                p->js_fd = fd; p->device_index = i; str_copy(p->device_name, name, sizeof p->device_name); break;
            }
        }
        if (p->js_fd < 0 && fallback_fd >= 0) {
            p->js_fd = fallback_fd; p->device_index = fallback_index;
            str_copy(p->device_name, fallback_name, sizeof p->device_name);
        } else if (fallback_fd >= 0) close(fallback_fd);
    }
    if (p->js_fd < 0) {
        char fail[96]; int err = *__errno_location();
        p->capture_exclusive = 0; str_copy(p->capture_status, "no selected device", sizeof p->capture_status);
        snprintf(fail, sizeof fail, "input joystick=open-failed index=%d errno=%d", wanted, err);
        diag_rl(2, fail); return;
    }
    p->selected_js_index = p->device_index;
    if (!p->selected_usb_id[0]) usb_identity_for_js(p->device_index, p->selected_usb_id, sizeof p->selected_usb_id);
    if (p->selected_usb_id[0]) save_input_config();
    pad_apply_mapping(p);
    p->axis_map_ok = ioctl(p->js_fd, JSIOCGAXMAP, p->axis_map) == 0;
    p->button_map_ok = ioctl(p->js_fd, JSIOCGBTNMAP, p->button_map) == 0;
    ioctl(p->js_fd, JSIOCGAXES, &p->axis_count);
    ioctl(p->js_fd, JSIOCGBUTTONS, &p->button_count);
    p->hat_x_axis = p->hat_y_axis = -1;
    if (p->axis_map_ok) {
        for (int i = 0; i < p->axis_count && i < 64; i++) {
            if (p->axis_map[i] == ABS_X) p->x_axis = i;
            else if (p->axis_map[i] == ABS_Y) p->y_axis = i;
            else if (p->axis_map[i] == ABS_HAT0X) p->hat_x_axis = i;
            else if (p->axis_map[i] == ABS_HAT0Y) p->hat_y_axis = i;
        }
    }
    if (p->hat_x_axis < 0 && p->axis_count >= 6) { p->hat_x_axis = 4; p->hat_y_axis = 5; }
    else if (p->hat_x_axis < 0 && p->axis_count >= 4) { p->hat_x_axis = 2; p->hat_y_axis = 3; }
    if (!p->button_map_ok) {
        p->button_map[0] = BTN_A; p->button_map[1] = BTN_B;
        p->button_map[8] = BTN_SELECT; p->button_map[9] = BTN_START;
    }
    /* Default bindings apply only to pad inputs the user has not mapped, so
       stick clicks reported as Select/Start do not return after a remap. */
    for (int i = 0; i < 64; i++) if (!p->custom_mapped[i]) {
        unsigned int pad = js_button_to_pad(p->button_map[i]);
        if (pad && target_user_mapped(p, pad)) pad = 0;
        p->button_pad[i] = (unsigned char)pad;
    }

    /* Match through sysfs parent links, then open only the selected pad's
       event node. Never probe/grab unrelated nodes such as the TV remote. */
    p->capture_exclusive = 0;
    str_copy(p->capture_status, "shared; Tizen shortcuts may still fire", sizeof p->capture_status);
    char js_parent[256], event_parent[256], path[96], diag[192];
    int matching_event = -1;
    js_parent[0] = 0;
    if (sysfs_input_parent("js", p->device_index, js_parent, sizeof js_parent)) {
        for (int e = 0; e < 32; e++) {
            if (sysfs_input_parent("event", e, event_parent, sizeof event_parent) &&
                strncmp(js_parent, event_parent, sizeof js_parent) == 0) {
                matching_event = e; break;
            }
        }
    }
    if (matching_event >= 0 && g_exclusive_preferred) {
        snprintf(path, sizeof path, TIZGB_DEV_INPUT "/event%d", matching_event);
        /* access() shows whether the root watcher has granted this node yet,
           without a shell on the TV (TizNES 1.0.23 diagnostics). */
        int can_r = access(path, R_OK) == 0, can_w = access(path, W_OK) == 0;
        int fd = open(path, O_RDONLY|O_NONBLOCK);
        if (fd < 0) {
            int err = *__errno_location();
            snprintf(diag, sizeof diag, "input evdev=candidate=%d open_errno=%d access_r=%d access_w=%d",
                matching_event, err, can_r, can_w);
            snprintf(p->capture_status, sizeof p->capture_status, "shared; event%d access denied (%d)", matching_event, err);
            diag_rl(0, diag);
        } else {
            int grab = 1;
            if (ioctl(fd, EVIOCGRAB, &grab) == 0) {
                p->ev_fd = fd; p->capture_exclusive = 1;
                for (int axis = 0; axis < ABS_CODE_COUNT; axis++) {
                    input_absinfo info;
                    if (ioctl(p->ev_fd, EVIOCGABS(axis), &info) == 0 && info.maximum > info.minimum) {
                        p->ev_abs_min[axis] = info.minimum;
                        p->ev_abs_max[axis] = info.maximum;
                        p->ev_abs_range_ok[axis] = 1;
                    }
                }
                str_copy(p->capture_status, "exclusive capture active", sizeof p->capture_status);
                snprintf(diag, sizeof diag, "input evdev=exclusive event=%d", matching_event);
                diag_rl(0, diag);
            } else {
                int err = *__errno_location();
                snprintf(diag, sizeof diag, "input evdev=candidate=%d grab_errno=%d access_r=%d access_w=%d",
                    matching_event, err, can_r, can_w);
                snprintf(p->capture_status, sizeof p->capture_status, "shared; event%d grab denied (%d)", matching_event, err);
                diag_rl(0, diag); close(fd);
            }
        }
    } else if (matching_event < 0) {
        char nodiag[256];
        snprintf(nodiag, sizeof nodiag, "input evdev=no-sysfs-match js_parent=%.200s", js_parent[0] ? js_parent : "(unresolved)");
        diag_rl(0, nodiag);
    } else {
        diag_rl(0, "input evdev=exclusive-capture-disabled-by-setting");
    }
    p->ev_device_index = matching_event;
    char line[224];
    snprintf(line, sizeof line, "input js%d=open name=%s axes=%u buttons=%u exclusive=%d",
        p->device_index, p->device_name, p->axis_count, p->button_count, p->capture_exclusive);
    diag_rl(1, line);
}

static void js_log_first_event(pad_t *p, int is_button, unsigned int number, unsigned short code, short value, unsigned int pad_bit)
{
    unsigned char *seen = is_button ? p->button_logged : p->axis_logged;
    if (number >= 64 || seen[number]) return;
    seen[number] = 1;
    char line[128];
    snprintf(line, sizeof line, "input js%d %s index=%u code=0x%x value=%d pad_bit=0x%x",
        p->device_index, is_button ? "button" : "axis", number, code, value, pad_bit);
    gbp_diag(line);
}

static void mapping_advance(pad_t *p)
{
    p->button_bits = p->stick_bits = p->hat_bits = p->mapped_axis_bits = 0;
    p->ev_button_bits = 0;
    for (int i = 0; i < 64; i++) p->axis_state[i] = 0;
    pad_save_model_mapping(p);
    g_map_step++;
    if (g_map_step >= 8) {
        g_state = ST_SETTINGS;
        str_copy(g_settings_message, "Gamepad mapping saved", sizeof g_settings_message);
        show_group(g_state);
    } else render_mapping();
}

static void mapping_clear_target(pad_t *p, unsigned char target)
{
    for (int i = 0; i < 64; i++) {
        p->button_pad[i] &= (unsigned char)~target;
        p->axis_neg_pad[i] &= (unsigned char)~target;
        p->axis_pos_pad[i] &= (unsigned char)~target;
    }
    for (int code = 0; code < EV_KEY_CODE_COUNT; code++) p->ev_key_pad[code] &= (unsigned char)~target;
}

static const unsigned char g_map_targets[8] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_SELECT, PAD_START};
static int pad_is_mapping(const pad_t *p) { (void)p; return g_state == ST_MAPPING && g_map_step >= 0 && g_map_step < 8; }

static void js_button_event(pad_t *p, unsigned int index, unsigned short code, int value)
{
    unsigned int bit = index < 64 ? p->button_pad[index] : 0;
    js_log_first_event(p, 1, index, code, (short)value, bit);
    if (pad_is_mapping(p) && index < 64) {
        if (!value && !g_map_armed) { g_map_armed = 1; render_mapping(); return; }
        if (value == 1 && g_map_armed) {
            mapping_clear_target(p, g_map_targets[g_map_step]);
            p->button_pad[index] = g_map_targets[g_map_step];
            p->custom_mapped[index] = 1;
            snprintf(g_map_detected, sizeof g_map_detected, "button %u (code 0x%x)", index, code);
            g_map_armed = 0;
            mapping_advance(p);
        }
        return;
    }
    if (code == BTN_MODE && (index >= 64 || !p->custom_mapped[index])) {
        if (value) {
            if (g_state == ST_LIBRARY) settings_open();
            else if (g_state == ST_GAME) help_open(ST_GAME);
            else if (g_state == ST_SETTINGS) settings_close();
            else if (g_state == ST_HELP) help_close();
        }
        return;
    }
    if (bit) { if (value) p->button_bits |= bit; else p->button_bits &= ~bit; }
}

/* The evdev key code is the device's stable identity; keep a direct map as
   well as the joydev-index map (some drivers emit codes with no BTNMAP). */
static void ev_key_event(pad_t *p, unsigned short code, int value)
{
    if (pad_is_mapping(p)) {
        if (value == 0 && !g_map_armed) { g_map_armed = 1; render_mapping(); return; }
        if (value == 1 && g_map_armed && code < EV_KEY_CODE_COUNT) {
            mapping_clear_target(p, g_map_targets[g_map_step]);
            for (unsigned int i = 0; i < p->button_count && i < 64; i++) if (p->button_map[i] == code) {
                p->button_pad[i] = 0;
                p->custom_mapped[i] = 1;
            }
            p->ev_key_pad[code] = g_map_targets[g_map_step];
            snprintf(g_map_detected, sizeof g_map_detected, "event code 0x%x", code);
            g_map_armed = 0;
            mapping_advance(p);
        }
        return;
    }
    if (code < EV_KEY_CODE_COUNT && p->ev_key_pad[code]) {
        if (value) p->ev_button_bits |= p->ev_key_pad[code];
        else p->ev_button_bits &= ~p->ev_key_pad[code];
    }
}

/* Axes are identified by their Linux ABS code, never by a joydev index (the
   numberings differ; mixing them made ABS_RZ look like the D-pad hat in
   TizNES 1.0.21 and held Down). */
static void js_axis_event(pad_t *p, unsigned int number, unsigned short code, int value, int evdev)
{
    int is_hat = (code == ABS_HAT0X || code == ABS_HAT0Y);
    int direction = 0;
    if (is_hat) direction = value < 0 ? -1 : (value > 0 ? 1 : 0);
    else if (evdev && code < ABS_CODE_COUNT && p->ev_abs_range_ok[code]) {
        int min = p->ev_abs_min[code], max = p->ev_abs_max[code];
        int center = min + (max-min)/2, dead = (max-min)/4;
        if (value < center-dead) direction = -1;
        else if (value > center+dead) direction = 1;
    } else if (value <= -JS_DEAD) direction = -1;
    else if (value >= JS_DEAD) direction = 1;
    if (pad_is_mapping(p)) {
        if (!direction && !g_map_armed) { g_map_armed = 1; render_mapping(); return; }
        if (number < 64 && g_map_step < 4 && direction && g_map_armed) {
            mapping_clear_target(p, g_map_targets[g_map_step]);
            if (direction < 0) p->axis_neg_pad[number] = g_map_targets[g_map_step];
            else p->axis_pos_pad[number] = g_map_targets[g_map_step];
            snprintf(g_map_detected, sizeof g_map_detected, "axis %u %s (code 0x%x)", number, direction < 0 ? "negative" : "positive", code);
            g_map_armed = 0;
            mapping_advance(p);
            return;
        }
    }
    if (number < 64 && (p->axis_neg_pad[number] || p->axis_pos_pad[number])) {
        p->axis_state[number] = (signed char)direction;
        p->mapped_axis_bits = 0;
        for (int i = 0; i < 64; i++) {
            if (p->axis_state[i] < 0) p->mapped_axis_bits |= p->axis_neg_pad[i];
            else if (p->axis_state[i] > 0) p->mapped_axis_bits |= p->axis_pos_pad[i];
        }
        js_log_first_event(p, 0, number, code, (short)value, 0);
        return;
    }
    /* Unmapped axes: only the left stick and the D-pad hat steer. */
    unsigned int *axis_bits = 0, neg = 0, pos = 0;
    if (code == ABS_X) { axis_bits = &p->stick_bits; neg = PAD_LEFT; pos = PAD_RIGHT; }
    else if (code == ABS_Y) { axis_bits = &p->stick_bits; neg = PAD_UP; pos = PAD_DOWN; }
    else if (code == ABS_HAT0X) { axis_bits = &p->hat_bits; neg = PAD_LEFT; pos = PAD_RIGHT; }
    else if (code == ABS_HAT0Y) { axis_bits = &p->hat_bits; neg = PAD_UP; pos = PAD_DOWN; }
    js_log_first_event(p, 0, number, code, (short)value, 0);
    if (axis_bits) {
        *axis_bits &= ~(neg|pos);
        if (direction < 0) *axis_bits |= neg;
        else if (direction > 0) *axis_bits |= pos;
    }
}

/* After SYN_DROPPED, rebuild held keys/axes from the device's state. */
static void ev_resync(pad_t *p)
{
    unsigned char keys[96];
    if (p->ev_fd < 0) return;
    for (int i = 0; i < (int)sizeof keys; i++) keys[i] = 0;
    p->ev_button_bits = 0; p->button_bits = 0;
    if (ioctl(p->ev_fd, EVIOCGKEY(sizeof keys), keys) == 0) {
        for (int code = 0; code < EV_KEY_CODE_COUNT && code < (int)sizeof keys*8; code++) {
            if (!(keys[code>>3] & (1<<(code&7)))) continue;
            p->ev_button_bits |= p->ev_key_pad[code];
            for (unsigned int i = 0; i < p->button_count && i < 64; i++)
                if (p->button_map[i] == code) p->button_bits |= p->button_pad[i];
        }
    }
    if (!pad_is_mapping(p) && g_state != ST_INPUT_TEST) {
        for (int axis = 0; axis < ABS_CODE_COUNT; axis++) {
            input_absinfo info;
            if (p->ev_abs_range_ok[axis] && ioctl(p->ev_fd, EVIOCGABS(axis), &info) == 0)
                js_axis_event(p, (unsigned int)axis, (unsigned short)axis, info.value, 1);
        }
    }
    gbp_diag("input evdev=resync-after-dropped-events");
}

static void pad_poll(pad_t *p)
{
    if (p->ev_fd < 0 && g_exclusive_preferred && p->js_fd >= 0 && g_input_frame >= p->capture_retry_frame) {
        int selected = p->device_index;
        p->capture_retry_frame = g_input_frame + PAD_RETRY_FRAMES;
        js_open_device(p, selected);
    }
    if (p->js_fd < 0) {
        if (g_input_frame < p->open_retry_frame) return;
        p->open_retry_frame = g_input_frame + PAD_RETRY_FRAMES;
        if (p->selected_usb_id[0]) p->selected_js_index = pad_resolve_index(p);
        js_open_device(p, p->selected_js_index);
        if (p->js_fd < 0) return;
    }
    int test_mode = g_state == ST_INPUT_TEST;
    int mapping_mode = g_state == ST_MAPPING;
    long n = -1;
    /* Only a shared (joydev) pad stamps g_js_activity_frame: Tizen also
       turns its buttons into navigation keys, which on_key_down then drops
       as duplicates. A grabbed pad never reaches Tizen, so its activity
       (e.g. resting-stick jitter) must not swallow remote keys (1.0.26). */
    if (p->ev_fd >= 0) {
        input_event_t e;
        int dropped = 0;
        while ((n = read(p->ev_fd, &e, sizeof e)) == (long)sizeof e) {
            if (e.type == EV_SYN && e.code == SYN_DROPPED) { dropped = 1; continue; }
            if (dropped) {
                if (e.type == EV_SYN && e.code == SYN_REPORT) { dropped = 0; ev_resync(p); }
                continue;
            }
            if (test_mode && (e.type == EV_KEY || e.type == EV_ABS)) {
                int index = -1;
                if (e.type == EV_KEY) for (unsigned int i = 0; i < p->button_count && i < 64; i++)
                    if (p->button_map[i] == e.code) { index = (int)i; break; }
                input_test_event(e.type, e.code, e.value, index, "evdev");
            } else if (mapping_mode && e.type == EV_KEY) {
                ev_key_event(p, e.code, e.value);
            } else if (e.type == EV_KEY) {
                ev_key_event(p, e.code, e.value);
                for (unsigned int i = 0; i < p->button_count && i < 64; i++)
                    if (p->button_map[i] == e.code) { js_button_event(p, i, e.code, e.value); break; }
            } else if (e.type == EV_ABS) {
                js_axis_event(p, e.code, e.code, e.value, 1);
            }
        }
    } else {
        js_event e;
        while ((n = read(p->js_fd, &e, sizeof e)) == (long)sizeof e) {
            g_js_activity_frame = g_input_frame; g_js_activity_valid = 1;
            unsigned char t = e.type & 0x7f;
            if (t == 0x01 && test_mode)
                input_test_event(EV_KEY, e.number < 64 ? p->button_map[e.number] : 0, e.value, e.number, "joydev");
            else if (t == 0x02 && test_mode)
                input_test_event(EV_ABS, e.number < 64 ? p->axis_map[e.number] : 0xffff, e.value, e.number, "joydev");
            else if (t == 0x01) js_button_event(p, e.number, e.number < 64 ? p->button_map[e.number] : 0, e.value);
            else if (t == 0x02) {
                unsigned short axis_code = (e.number < 64 && p->axis_map_ok) ? p->axis_map[e.number] : e.number;
                js_axis_event(p, axis_code, axis_code, e.value, 0);
            }
        }
    }
    if (n == 0 || (n < 0 && *__errno_location() != EAGAIN && *__errno_location() != EINTR)) {
        gbp_diag("input device=disconnected"); js_close_device(p);
        p->button_bits = p->stick_bits = p->hat_bits = 0;
    }
    p->jsbits = p->button_bits | p->stick_bits | p->hat_bits | p->mapped_axis_bits | p->ev_button_bits;
}

/* Set by app_exit: events and animator ticks that arrive before the main
   loop stops must not reopen (and re-grab) the controller. */
static int g_exiting;
static void js_poll(void) { if (!g_exiting) pad_poll(PAD1); }

static Eina_Bool animate(void *data)
{
    (void)data;
    if (g_exiting) return 1;
    g_input_frame++;
    js_poll();
    library_tick(g_input_frame);
    unsigned int ctl = PAD1->jsbits;
    if (g_state == ST_GAME) {
        unsigned int pad = g_keybits | ctl;
        if ((int)pad != g_game_input_log_bits) {
            char line[128];
            snprintf(line, sizeof line, "game-input pad=0x%x keyboard=0x%x controller=0x%x", pad, g_keybits, ctl);
            gbp_diag(line);
            g_game_input_log_bits = (int)pad;
        }
    }
    unsigned int nav_bits = resolve_opposing_directions(ctl);
    unsigned int pressed = nav_bits & ~g_js_prevbits;
    if (g_state == ST_LIBRARY) {
        if (pressed & PAD_UP) library_move(-1);
        else if (pressed & PAD_DOWN) library_move(1);
        else if (pressed & PAD_LEFT) library_move(-VIS_ROWS);
        else if (pressed & PAD_RIGHT) library_move(VIS_ROWS);
        else if (pressed & PAD_A) { library_activate(); }
        else if (pressed & PAD_START) settings_open();
        else if (pressed & PAD_SELECT) help_open(ST_LIBRARY);
    } else if (g_state == ST_SETTINGS) {
        if (pressed & PAD_UP) { if (g_settings_sel > 0) g_settings_sel--; g_settings_message[0] = 0; render_settings(); }
        else if (pressed & PAD_DOWN) { if (g_settings_sel < SETTINGS_ROWS-1) g_settings_sel++; g_settings_message[0] = 0; render_settings(); }
        else if (pressed & PAD_LEFT) settings_adjust(-1);
        else if (pressed & PAD_RIGHT) settings_adjust(1);
        else if (pressed & PAD_A) settings_activate();
        else if (pressed & (PAD_B|PAD_START)) settings_close();
    } else if (g_state == ST_DEVICES) {
        if (pressed & PAD_UP) { if (g_device_sel > 0) g_device_sel--; render_devices(); }
        else if (pressed & PAD_DOWN) { if (g_device_sel < g_device_count-1) g_device_sel++; render_devices(); }
        else if (pressed & PAD_A) device_select_activate();
        else if (pressed & (PAD_B|PAD_START)) { g_state = ST_SETTINGS; show_group(g_state); }
    } else if (g_state == ST_HELP) {
        if (pressed & (PAD_A|PAD_B|PAD_START|PAD_SELECT)) help_close();
    } else if (g_state == ST_ERROR) {
        if (pressed & (PAD_A|PAD_B|PAD_START)) return_to_library();
    }
    g_js_prevbits = nav_bits;
    if (g_state != ST_GAME) return 1;
    gbp_set_pad(resolve_opposing_directions(g_keybits | ctl));
    /* Run as many frames as the clock says are due (round to nearest so a
       tick that lands a little early still gets its frame). Usually 1; 0
       or 2 when the compositor rate differs from 59.7275 Hz. */
    unsigned long long now = mono_ns();
    unsigned long long period = g_pace_period_ns;
    if (!now) now = (g_pace_done + 1) * period;   /* clock failure: 1 frame/tick */
    if (!g_pace_t0) {
        g_pace_t0 = now; g_pace_done = 0;
        g_pace_win_t0 = now; g_pace_win_frames = 0;
    }
    unsigned long long due = (now - g_pace_t0 + period / 2) / period + 1;
    if (due > g_pace_done + PACE_MAX_LAG) {
        /* Stalled (app paused, heavy load): skip the backlog instead of
           fast-forwarding through it. */
        g_pace_t0 = now - g_pace_done * period;
        due = g_pace_done + 1;
        g_pace_stalls++;
    }
    int ran = 0;
    while (g_pace_done < due && ran < PACE_MAX_PER_TICK) {
        if (gbp_run_frame() != 0) {
            char reason[200]; str_copy(reason, gbp_last_error(), sizeof reason);
            gbp_stop();
            show_error("The game stopped with an emulation error.", reason);
            return 1;
        }
        g_pace_done++;
        ran++;
    }
    g_pace_ticks++;
    g_pace_win_frames += (unsigned long long)ran;
    if (!ran) g_pace_idle++;
    else if (ran > 1) g_pace_multi++;
    if (now - g_pace_win_t0 >= 60000000000ULL) {
        char line[200];
        unsigned long long span = now - g_pace_win_t0;
        unsigned int fps_x100 = (unsigned int)(g_pace_win_frames * 100000000000ULL / span);
        snprintf(line, sizeof line, "pace period_ns=%u emu_fps_x100=%u ticks=%lu idle=%lu multi=%lu stalls=%lu",
            g_pace_period_ns, fps_x100, g_pace_ticks, g_pace_idle, g_pace_multi, g_pace_stalls);
        gbp_diag(line);
        g_pace_win_t0 = now; g_pace_win_frames = 0;
    }
    if (ran) {
        evas_object_image_data_set(g_img, gbp_framebuffer());
        evas_object_image_data_update_add(g_img, 0, 0, gbp_width(), gbp_height());
    }
    if (g_perf_mode != PERF_OVERLAY_OFF && ran) {
        unsigned long long end = mono_ns();
        unsigned long long work = end > now ? (end - now) / (unsigned long long)ran : 0;
        int published = 0;
        for (int k = 0; k < ran; k++) published |= perf_frame(&g_perf, end, work, k > 0);
        if (published) perf_overlay_apply();
    }
    return 1; /* ECORE_CALLBACK_RENEW */
}

/* ------------------------------ input keys -------------------------------- */
static unsigned int key_to_pad(const char *kn)
{
    if (!kn) return 0;
    if (!strcasecmp(kn, "Up"))    return PAD_UP;
    if (!strcasecmp(kn, "Down"))  return PAD_DOWN;
    if (!strcasecmp(kn, "Left"))  return PAD_LEFT;
    if (!strcasecmp(kn, "Right")) return PAD_RIGHT;
    if (!strcasecmp(kn, "x"))     return PAD_A;
    if (!strcasecmp(kn, "z"))     return PAD_B;
    if (!strcasecmp(kn, "a"))     return PAD_SELECT;
    if (!strcasecmp(kn, "s"))     return PAD_START;
    if (!strcasecmp(kn, "Return") || !strcasecmp(kn, "KP_Enter")) return PAD_A;
    return 0;
}
static int is_back(const char *kn)
{
    return kn && (!strcasecmp(kn,"XF86Back") || !strcasecmp(kn,"BackSpace") ||
                  !strcasecmp(kn,"Escape") || !strcasecmp(kn,"XF86AudioStop"));
}
static int is_menu(const char *kn)
{
    return kn && (!strcasecmp(kn,"Menu") || !strcasecmp(kn,"XF86Menu") ||
                  !strcasecmp(kn,"XF86Tools") || !strcasecmp(kn,"F1"));
}
static int is_navigation_key(const char *kn)
{
    return kn && (!strcasecmp(kn,"Up") || !strcasecmp(kn,"Down") ||
                  !strcasecmp(kn,"Left") || !strcasecmp(kn,"Right") ||
                  !strcasecmp(kn,"Return") || !strcasecmp(kn,"KP_Enter") ||
                  is_back(kn) || is_menu(kn));
}
static int joystick_key_was_recent(void)
{
    return g_js_activity_valid && g_input_frame - g_js_activity_frame <= 3;
}

/* Quit the whole process: save, release the controller grab, ask the app
   framework to leave its main loop, and force the exit if it does not
   finish promptly (TizNES 1.0.22 Exit pattern). */
static Eina_Bool force_exit(void *data)
{
    (void)data;
    _exit(0);
    return 0;
}

static void app_exit(void)
{
    gbp_diag("exit requested from settings");
    g_exiting = 1;
    gbp_stop();
    js_close_device(PAD1);
    PAD1->jsbits = PAD1->button_bits = PAD1->stick_bits = PAD1->hat_bits = 0;
    gbp_set_pad(0);
    ecore_timer_add(2.0, force_exit, 0);
    ui_app_exit();
}

static void settings_open(void)
{
    g_settings_sel = 0; g_settings_message[0] = 0; g_state = ST_SETTINGS; show_group(g_state);
}

static void settings_close(void)
{
    return_to_library();
}

static void help_open(int return_state)
{
    g_help_return_state = return_state; g_state = ST_HELP; show_group(g_state);
}

static void help_close(void)
{
    g_state = g_help_return_state; show_group(g_state);
    if (g_state == ST_GAME) g_pace_t0 = 0;   /* resume pacing without a burst */
}

static void settings_adjust(int direction)
{
    char line[128];
    if (g_settings_sel == S_VOLUME) {
        g_volume += direction*10; if (g_volume < 0) g_volume = 0; if (g_volume > 100) g_volume = 100;
        gbp_audio_set_volume(g_volume); save_settings();
    } else if (g_settings_sel == S_MUTE) { g_muted = !g_muted; gbp_audio_set_mute(g_muted); save_settings(); }
    else if (g_settings_sel == S_AUDIO_TEST) {
        g_audio_test_filter = (g_audio_test_filter + direction + 5) % 5;
        gbp_audio_set_channel_filter(g_audio_test_filter);
        snprintf(line, sizeof line, "audio-test-filter=%d name=%s", g_audio_test_filter, g_audio_test_names[g_audio_test_filter]);
        gbp_diag(line);
    } else if (g_settings_sel == S_SCALE) {
        g_scale_mode = (g_scale_mode + direction + 3) % 3; apply_video_settings(); save_settings();
    } else if (g_settings_sel == S_FILTER) {
        g_smooth = !g_smooth; apply_video_settings(); save_settings();
    } else if (g_settings_sel == S_DMG_PALETTE) {
        int n = gbp_dmg_palette_count();
        g_dmg_palette = (g_dmg_palette + direction + n) % n; gbp_set_dmg_palette(g_dmg_palette); save_settings();
    } else if (g_settings_sel == S_CGB_COLORS) {
        g_cgb_colors = !g_cgb_colors; gbp_set_cgb_colors(g_cgb_colors); save_settings();
    } else if (g_settings_sel == S_PERF_OVERLAY) {
        g_perf_mode = (g_perf_mode + direction + PERF_OVERLAY_MODES) % PERF_OVERLAY_MODES; save_settings();
    } else return;
    g_settings_message[0] = 0; render_settings();
}

static void pad_reopen(pad_t *p)
{
    if (p->selected_usb_id[0]) p->selected_js_index = pad_resolve_index(p);
    js_open_device(p, p->selected_js_index);
}

static void mapping_start(void)
{
    pad_t *p = PAD1;
    if (p->js_fd < 0) pad_reopen(p);
    if (p->js_fd < 0) {
        str_copy(g_settings_message, "Select a readable controller first", sizeof g_settings_message);
        render_settings();
        return;
    }
    g_map_step = 0; g_map_armed = 1; g_map_detected[0] = 0;
    p->button_bits = p->stick_bits = p->hat_bits = p->mapped_axis_bits = 0;
    p->ev_button_bits = 0;
    g_state = ST_MAPPING; show_group(g_state);
}

static void settings_activate(void)
{
    switch (g_settings_sel) {
    case S_DEVICE: device_select_open(); break;
    case S_EXCLUSIVE:
        g_exclusive_preferred = !g_exclusive_preferred;
        save_input_config();
        js_open_device(PAD1, PAD1->selected_js_index);
        str_copy(g_settings_message, g_exclusive_preferred ? "Exclusive capture requested" : "Exclusive capture disabled", sizeof g_settings_message);
        render_settings();
        break;
    case S_MAP: mapping_start(); break;
    case S_INPUT_TEST:
        if (PAD1->js_fd < 0) js_open_device(PAD1, PAD1->selected_js_index);
        if (PAD1->js_fd < 0) { str_copy(g_settings_message, "Select a readable controller first", sizeof g_settings_message); render_settings(); }
        else input_test_start();
        break;
    case S_RESCAN: {
        library_rescan("settings");
        const lib_stats *st = lib_last_stats();
        if (g_library_message[0]) str_copy(g_settings_message, g_library_message, sizeof g_settings_message);
        else snprintf(g_settings_message, sizeof g_settings_message,
                      "ROM scan complete: %d found (%d on %d USB drive(s), %d folders searched)",
                      g_romcount, st->external_roms, st->external_roots, st->dirs);
        render_settings();
        break;
    }
    case S_EXIT: app_exit(); break;
    case S_BACK: settings_close(); break;
    default: settings_adjust(1);
    }
}

static void device_select_open(void)
{
    scan_js_devices();
    g_state = ST_DEVICES; show_group(g_state);
}

static void device_select_activate(void)
{
    if (g_device_sel < 0 || g_device_sel >= g_device_count) return;
    pad_t *p = PAD1;
    int index = g_device_indices[g_device_sel];
    char line[224], chosen[160];
    g_state = ST_SETTINGS;
    str_copy(chosen, g_device_usb_ids[g_device_sel], sizeof chosen);
    if (!identity_matches(p->selected_usb_id, chosen)) { p->map_model[0] = 0; p->map_loaded = 0; }
    p->selected_js_index = index;
    str_copy(p->selected_usb_id, chosen, sizeof p->selected_usb_id);
    js_open_device(p, index);
    save_input_config();
    snprintf(line, sizeof line, "input selection identity=%s js=%d", chosen, index);
    gbp_diag(line);
    if (p->js_fd < 0) str_copy(g_settings_message, "Could not open selected device", sizeof g_settings_message);
    else if (!p->capture_exclusive)
        str_copy(g_settings_message, g_exclusive_preferred ? "Selected; waiting for exclusive access" : "Shared input: Tizen shortcuts may still fire", sizeof g_settings_message);
    else str_copy(g_settings_message, "Exclusive capture active", sizeof g_settings_message);
    show_group(g_state);
}

static Eina_Bool on_key_down(void *d, int t, void *ev)
{
    Ecore_Event_Key *e = (Ecore_Event_Key *)ev; (void)d; (void)t;
    const char *kn = e ? e->keyname : 0;
    js_poll();
    if (g_state != ST_INPUT_TEST && is_navigation_key(kn) && joystick_key_was_recent()) return 1;
    if (g_state == ST_LIBRARY) {
        if (is_menu(kn) || (kn && (!strcasecmp(kn,"o") || !strcasecmp(kn,"O")))) settings_open();
        else if (kn && !strcasecmp(kn,"h")) help_open(ST_LIBRARY);
        else if (kn && !strcasecmp(kn,"Up"))   library_move(-1);
        else if (kn && !strcasecmp(kn,"Down")) library_move(1);
        else if (kn && (!strcasecmp(kn,"Left") || !strcasecmp(kn,"XF86RaiseChannel"))) library_move(-VIS_ROWS);
        else if (kn && (!strcasecmp(kn,"Right") || !strcasecmp(kn,"XF86LowerChannel"))) library_move(VIS_ROWS);
        else if (kn && (!strcasecmp(kn,"Return") || !strcasecmp(kn,"KP_Enter"))) { library_activate(); }
        return 1;
    }
    if (g_state == ST_SETTINGS) {
        if (is_back(kn)) { settings_close(); return 1; }
        if (kn && !strcasecmp(kn,"Up")) { if (g_settings_sel > 0) g_settings_sel--; g_settings_message[0] = 0; render_settings(); }
        else if (kn && !strcasecmp(kn,"Down")) { if (g_settings_sel < SETTINGS_ROWS-1) g_settings_sel++; g_settings_message[0] = 0; render_settings(); }
        else if (kn && !strcasecmp(kn,"Left")) settings_adjust(-1);
        else if (kn && !strcasecmp(kn,"Right")) settings_adjust(1);
        else if (kn && (!strcasecmp(kn,"Return") || !strcasecmp(kn,"KP_Enter"))) settings_activate();
        return 1;
    }
    if (g_state == ST_DEVICES) {
        if (is_back(kn)) { g_state = ST_SETTINGS; show_group(g_state); return 1; }
        if (kn && !strcasecmp(kn,"Up")) { if (g_device_sel > 0) g_device_sel--; render_devices(); }
        else if (kn && !strcasecmp(kn,"Down")) { if (g_device_sel < g_device_count-1) g_device_sel++; render_devices(); }
        else if (kn && (!strcasecmp(kn,"Return") || !strcasecmp(kn,"KP_Enter"))) device_select_activate();
        return 1;
    }
    if (g_state == ST_MAPPING) {
        if (is_back(kn)) { g_map_step = -1; PAD1->button_bits = 0; PAD1->ev_button_bits = 0; g_state = ST_SETTINGS; str_copy(g_settings_message, "Mapping cancelled", sizeof g_settings_message); show_group(g_state); }
        return 1;
    }
    if (g_state == ST_INPUT_TEST) {
        if (is_back(kn)) input_test_finish(1);
        else if (kn && (!strcasecmp(kn,"Return") || !strcasecmp(kn,"KP_Enter"))) input_test_skip();
        return 1;
    }
    if (g_state == ST_ERROR) { return_to_library(); return 1; }
    if (g_state == ST_HELP) {
        if (is_back(kn) || is_menu(kn) || (kn && !strcasecmp(kn,"h"))) help_close();
        return 1;
    }
    if (g_state == ST_GAME) {
        if (is_back(kn)) { stop_game(); return 1; }
        if (is_menu(kn) || (kn && !strcasecmp(kn,"h"))) { help_open(ST_GAME); return 1; }
        if (kn && !strcasecmp(kn,"m")) { gbp_audio_set_mute(!gbp_audio_is_muted()); return 1; }
        if (kn && !strcasecmp(kn,"comma"))  { gbp_audio_set_volume(gbp_audio_get_volume()-10); return 1; }
        if (kn && !strcasecmp(kn,"period")) { gbp_audio_set_volume(gbp_audio_get_volume()+10); return 1; }
        g_keybits |= key_to_pad(kn);
    }
    return 1;
}
static Eina_Bool on_key_up(void *d, int t, void *ev)
{
    Ecore_Event_Key *e = (Ecore_Event_Key *)ev; (void)d; (void)t;
    const char *kn = e ? e->keyname : 0;
    js_poll();
    if (is_navigation_key(kn) && joystick_key_was_recent()) return 1;
    if (g_state == ST_GAME) g_keybits &= ~key_to_pad(kn);
    return 1;
}

/* --------------------------------- setup ---------------------------------- */
static void pads_init(void)
{
    pad_t *p = PAD1;
    memset(p, 0, sizeof *p);
    p->slot = 0;
    p->js_fd = p->ev_fd = -1;
    p->x_axis = 0; p->y_axis = 1;
    p->hat_x_axis = p->hat_y_axis = -1;
    p->device_index = p->ev_device_index = p->selected_js_index = -1;
}

static int create(void *data)
{
    (void)data;
    pads_init();
    gbp_init();
    load_settings();
    load_input_config();
    if (PAD1->selected_usb_id[0]) PAD1->selected_js_index = pad_resolve_index(PAD1);

    g_win = elm_win_util_standard_add("tizgb", "TizGB");
    if (!g_win) return 0;
    elm_win_fullscreen_set(g_win, 1);
    evas_object_resize(g_win, TV_W, TV_H);
    g_canvas = evas_object_evas_get(g_win);

    g_bg = evas_object_rectangle_add(g_canvas);
    evas_object_color_set(g_bg, 12, 14, 20, 255);
    elm_win_resize_object_add(g_win, g_bg);
    evas_object_show(g_bg);

    g_img = evas_object_image_filled_add(g_canvas);
    evas_object_image_size_set(g_img, gbp_width(), gbp_height());
    evas_object_image_alpha_set(g_img, 0);
    evas_object_image_data_set(g_img, gbp_framebuffer());
    apply_video_settings();

    g_title  = mk_text(56, 255, 255, 255);  evas_object_move(g_title, 360, 120);
    g_footer = mk_text(30, 120, 200, 255);  evas_object_move(g_footer, 360, 1000);
    g_cursor = evas_object_rectangle_add(g_canvas);
    evas_object_color_set(g_cursor, 120, 200, 255, 255);
    for (int i = 0; i < VIS_ROWS; i++) g_row[i] = mk_text(34, 200, 200, 200);
    for (int i = 0; i < SETTINGS_ROWS; i++) g_set_row[i] = mk_text(30, 200, 200, 200);
    for (int i = 0; i < SETTINGS_SECTIONS; i++) {
        g_set_head[i] = mk_text(26, 120, 200, 255);
        g_set_rule[i] = evas_object_rectangle_add(g_canvas);
        evas_object_color_set(g_set_rule[i], 60, 90, 120, 255);
    }

    g_err = elm_label_add(g_win); evas_object_move(g_err, 360, 420);
    evas_object_resize(g_err, 1200, 300);

    g_help = elm_label_add(g_win); evas_object_move(g_help, 300, 260);
    evas_object_resize(g_help, 1320, 600);
    elm_object_part_text_set(g_help, 0,
        "<align=left><font_size=38><color=#ebebeb>"
        "Controls<br/><br/>"
        "D-pad: arrow keys, left stick, or controller D-pad<br/>"
        "A: X key / controller A    B: Z key / controller B<br/>"
        "Select: A key / controller Select    Start: S or Enter / Start button<br/><br/>"
        "Library: first row Exit TizGB closes the app; Left/Right pages;<br/>Start opens Settings, Select opens Help<br/>"
        "USB drives are searched 8 folders deep; games play from the drive<br/>"
        "and saves stay on the TV. Plug a drive in or out: the list updates.<br/>"
        "Settings: D-pad moves, Left/Right changes, A selects, B/Start returns<br/>"
        "System > Performance overlay shows FPS (or details) over the game<br/>"
        "In game: Back returns to the library (the game's save is written)<br/>"
        "O: Settings (keyboard)    M: mute    , / .: volume    H: help"
        "</color></font_size></align>");

    /* Created last: stays above the game image, and keeps earlier object order. */
    g_perf_bg = evas_object_rectangle_add(g_canvas);
    evas_object_color_set(g_perf_bg, 0, 0, 0, 160);
    g_perf_txt = mk_text(28, 255, 255, 255);

    elm_object_focus_set(g_win, 1);
    ecore_event_handler_add(ECORE_EVENT_KEY_DOWN, on_key_down, 0);
    ecore_event_handler_add(ECORE_EVENT_KEY_UP,   on_key_up,   0);

    js_open_device(PAD1, PAD1->selected_js_index);
    /* First scan from the animator, after "Scanning" is on screen. */
    g_sel = g_top = 0;
    library_request_scan("startup");
    g_scan_phase = 1;
    g_state = ST_LIBRARY;
    show_group(g_state);
    evas_object_show(g_win);
    g_anim = ecore_animator_add(animate, 0);
    return 1;
}

/* Leaving the foreground (Home, another app, TV standby): write the battery
   save now and release the controller so other apps can use it. */
static void on_app_pause(void *data)
{
    (void)data;
    gbp_save_now();
    js_close_device(PAD1);
    PAD1->jsbits = PAD1->button_bits = PAD1->stick_bits = PAD1->hat_bits = 0;
}

static void on_app_resume(void *data)
{
    (void)data;
    if (PAD1->js_fd < 0) pad_reopen(PAD1);
    g_pace_t0 = 0;   /* restart pacing from now instead of skipping */
    /* A drive may have come or gone while TizGB was in the background. */
    if (lib_roots_changed()) library_request_scan("resume");
}

static void on_app_terminate(void *data)
{
    (void)data;
    gbp_stop();
    js_close_device(PAD1);
}

int main(int argc, char **argv)
{
    callbacks_t cb = {(void *)create, (void *)on_app_terminate,
                      (void *)on_app_pause, (void *)on_app_resume, 0};
    return ui_app_main(argc, argv, &cb, 0);
}
