/*
 * platform.c - Walnut-CGB port for the Samsung Q60A native frontend (TizGB).
 *
 * Implements the core's callbacks against this TV's libraries and exposes a
 * small "gbp_*" bridge that the EFL frontend (gb_main.c) drives, mirroring
 * the nesp_* bridge of TizNES. Freestanding C: the only headers are the
 * shims in include/ (types plus hand-declared libc), libc.so.6 is linked.
 *
 * Video : lcd_draw_line() converts each 160-pixel line to ARGB8888. CGB games
 *         use their own BGR555 palette RAM (raw or LCD-corrected colours);
 *         DMG games use a selectable 3x4 palette (OBJ0, OBJ1, BG layers).
 * Audio : minigb_apu (patched, see vendor/Walnut-CGB/PATCHES.md) is rendered
 *         up to each APU register access's position inside the frame, so
 *         effects that change registers mid-frame keep their timing. Each
 *         emulated frame's 738-739 stereo samples are then resampled to that
 *         frame's share of the 44.1 kHz stream, trimmed by the ring fill
 *         (TizNES 1.0.24 pattern), DC-blocked, and queued for the S16LE
 *         capi-media-audio-io pull callback. The ring starts with silence
 *         queued at the target fill.
 * ROMs  : loaded completely into RAM straight from their path (app data
 *         folder or a USB drive; nothing is copied to internal storage), so
 *         a drive removed during play does not affect the running game. A
 *         failed or short read (drive pulled while loading) is an error,
 *         never a half-loaded game.
 * Saves : always on internal storage, in data/saves/<key>.sav and
 *         <key>.rtc (MBC3 clock), whatever drive the ROM came from. The key
 *         is the header title plus the 64-bit FNV-1a hash of the whole ROM
 *         file ("POKEMON_YELLOW-0123456789abcdef"): two different ROMs that
 *         share a file name (game.gbc on two pendrives) or even a header
 *         title never share a save, while the same ROM file keeps its save
 *         wherever it is found (another drive, renamed, a different USB port
 *         giving a different mount path). Written atomically ~2 s after the
 *         game stops writing, when leaving the game, on pause and on exit.
 *         With no internal save yet, a "<rom name>.sav" next to the ROM (as
 *         other emulators leave them) is imported read-only; the drive is
 *         never written.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---- hand-declared libc (resolved from the TV's libc.so.6) ---- */
extern int   open(const char *, int, ...);
extern long  read(int, void *, unsigned long);
extern long  write(int, const void *, unsigned long);
extern int   close(int);
extern int   fsync(int);
extern int   rename(const char *, const char *);
extern int   unlink(const char *);
extern int   mkdir(const char *, unsigned int);
extern int   snprintf(char *, unsigned long, const char *, ...);
extern long  lseek(int, long, int);
extern int  *__errno_location(void);
typedef struct { long long align; unsigned char space[1016]; } gbp_jmp_buf[1];
extern int   _setjmp(gbp_jmp_buf);
extern void  longjmp(gbp_jmp_buf, int) __attribute__((noreturn));

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_NOFOLLOW 0x8000   /* ARM; on the x86-64 host harness this is O_LARGEFILE (harmless) */
#define SEEK_SET 0
#define SEEK_END 2
#define E_NOENT 2
#define E_INTR  4
#define E_IO    5
#define E_NXIO  6
#define E_NODEV 19
#define E_STALE 116

/* ---- capi-media-audio-io (hand-declared; resolved at runtime) ---- */
typedef void *audio_out_h;
typedef void (*audio_out_stream_cb)(audio_out_h handle, unsigned int nbytes, void *user_data);
extern int audio_out_create_new(int sample_rate, int channel, int type, audio_out_h *output);
extern int audio_out_set_stream_cb(audio_out_h output, audio_out_stream_cb cb, void *user_data);
extern int audio_out_prepare(audio_out_h output);
extern int audio_out_unprepare(audio_out_h output);
extern int audio_out_destroy(audio_out_h output);
extern int audio_out_write(audio_out_h output, void *buffer, unsigned int length);
#define AUDIO_CHANNEL_MONO      0x80
#define AUDIO_CHANNEL_STEREO    0x81
#define AUDIO_SAMPLE_TYPE_S16LE 0x71

/* ---- core ---- */
#define AUDIO_SAMPLE_RATE 44100
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#include "../vendor/Walnut-CGB/minigb_apu/minigb_apu.h"
uint8_t audio_read(const uint16_t addr);
void audio_write(const uint16_t addr, const uint8_t val);
/* walnut_cgb.h prototypes struct gb_s functions before defining the struct
   (valid C++ but not C); declare the tag first. */
struct gb_s;
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define WALNUT_GB_12_COLOUR 1
#define WALNUT_FULL_GBC_SUPPORT 1
#include "../vendor/Walnut-CGB/walnut_cgb.h"
#undef MIN
#undef MAX
#include "../vendor/Walnut-CGB/minigb_apu/minigb_apu.c"

#ifndef TIZGB_DATA_DIR
#define TIZGB_DATA_DIR "/opt/usr/home/owner/apps_rw/org.pablo.tizgb/data"
#endif
#define DIAG_PATH TIZGB_DATA_DIR "/tizgb-runtime.log"
#define SAVE_DIR  TIZGB_DATA_DIR "/saves"

#define GB_W 160
#define GB_H 144
#define FRAME_CYCLES 70224u
#define GB_CLOCK     4194304u
#define MAX_ROM_BYTES (8u * 1024u * 1024u)

/* ================= diagnostics ================= */
void gbp_diag(const char *line)
{
    int fd = open(DIAG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    unsigned long n = strlen(line);
    write(fd, line, n);
    write(fd, "\n", 1);
    close(fd);
}

/* ================= state ================= */
static struct gb_s g_gb;
static int g_loaded;
static uint8_t *g_rom;
static unsigned long g_rom_size;
static uint8_t *g_cart_ram;
static size_t g_cart_ram_size;
static int g_cart_dirty;
static unsigned long g_cart_dirty_frame;
static unsigned long g_frames;
static char g_save_path[600], g_rtc_path[600];
static char g_save_key[48];
static int g_has_rtc;
static int g_error_io;     /* last load failed opening/reading the file */
static gbp_jmp_buf g_jmp;
static int g_in_frame;
static char g_error[160];

static unsigned int g_argb[GB_W * GB_H];
static volatile unsigned int g_pad;   /* active-high Walnut JOYPAD_* bits */

/* ================= video ================= */
/* DMG palettes: [layer][shade], layer 0 OBJ0, 1 OBJ1, 2 BG; shade 0 lightest. */
static unsigned int g_dmg_pal[3][4];
static int g_dmg_palette_mode = 2;    /* index into the table below */
static int g_cgb_color_mode = 1;      /* 0 raw, 1 LCD-corrected */
static unsigned int g_cgb_lut[32768];
static int g_cgb_lut_mode = -1;
static uint8_t g_colour_hash;

static unsigned int rgb555_argb(unsigned int c)   /* 0RRRRRGGGGGBBBBB */
{
    unsigned int r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* GBC palette RAM is little-endian BGR555 (red in the low bits). Raw scales
   5 to 8 bits; corrected approximates the GBC's dim, desaturated LCD with
   the widely used higan/bsnes colour matrix. */
static void cgb_build_lut(int mode)
{
    for (unsigned int c = 0; c < 32768; c++) {
        unsigned int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, R, G, B;
        if (mode == 0) {
            R = (r << 3) | (r >> 2); G = (g << 3) | (g >> 2); B = (b << 3) | (b >> 2);
        } else {
            R = r * 26 + g * 4 + b * 2;
            G = g * 24 + b * 8;
            B = r * 6 + g * 4 + b * 22;
            if (R > 960) R = 960;
            if (G > 960) G = 960;
            if (B > 960) B = 960;
            R >>= 2; G >>= 2; B >>= 2;
        }
        g_cgb_lut[c] = 0xff000000u | (R << 16) | (G << 8) | B;
    }
    g_cgb_lut_mode = mode;
}

/* The 12 palettes a Game Boy Color offers for monochrome games at boot,
   from Walnut-CGB extras/cgb.h (OBJ0, OBJ1, BG rows, RGB888). */
#include "../vendor/Walnut-CGB/extras/cgb.h"
static const char *g_gbc_combo_names[GBC_PALETTE_COUNT] = {
    "Down+A (red)", "Down+B (yellow)", "Down (pastel)", "Left+A (blue)",
    "Left+B (grey)", "Left (two-tone)", "Right+A (green)", "Right+B (inverted)",
    "Right (green-red)", "Up+A (mixed)", "Up+B (brown)", "Up (orange)"
};

/* Per-game GBC-style colours for known monochrome titles, keyed by the
   title checksum the GBC boot ROM uses (table from the Peanut-GB SDL
   example, MIT). RGB555, rows OBJ0, OBJ1, BG. Unknown titles get grey. */
static void auto_palette(uint8_t hash, uint16_t out[3][4])
{
    static const uint16_t grey[3][4] = {
        {0x7FFF,0x5294,0x294A,0x0000},{0x7FFF,0x5294,0x294A,0x0000},{0x7FFF,0x5294,0x294A,0x0000}};
    static const struct { uint8_t n, h[8]; uint16_t p[3][4]; } tab[] = {
        {2, {0x71,0xFF}, {{0x7FFF,0x7E60,0x7C00,0},{0x7FFF,0x7E60,0x7C00,0},{0x7FFF,0x7E60,0x7C00,0}}},
        {3, {0x15,0xDB,0x95}, {{0x7FFF,0x7FE0,0x7C00,0},{0x7FFF,0x7FE0,0x7C00,0},{0x7FFF,0x7FE0,0x7C00,0}}},
        {1, {0x19}, {{0x7FFF,0x7E10,0x48E7,0},{0x7FFF,0x7E10,0x48E7,0},{0x7FFF,0x7E60,0x7C00,0}}},
        {3, {0x61,0x45,0xD8}, {{0x7FFF,0x7E10,0x48E7,0},{0x7FFF,0x329F,0x001F,0},{0x7FFF,0x329F,0x001F,0}}},
        {1, {0x14}, {{0x7FFF,0x3FE6,0x0200,0},{0x7FFF,0x7E10,0x48E7,0},{0x7FFF,0x7E10,0x48E7,0}}},
        {1, {0x8B}, {{0x7FFF,0x7E10,0x48E7,0},{0x7FFF,0x329F,0x001F,0},{0x7FFF,0x3FE6,0x0200,0}}},
        {4, {0x27,0x49,0x5C,0xB3}, {{0x7D8A,0x6800,0x3000,0},{0x001F,0x7FFF,0x7FEF,0x021F},{0x527F,0x7FE0,0x0180,0}}},
        {4, {0x18,0x6A,0x4B,0x6B}, {{0x7F08,0x7F40,0x48E0,0x2400},{0x7FFF,0x2EFF,0x7C00,0x001F},{0x7FFF,0x463B,0x2951,0}}},
        {1, {0x70}, {{0x7FFF,0x03E0,0x1A00,0x0120},{0x7FFF,0x329F,0x001F,0x001F},{0x7FFF,0x7E10,0x48E7,0}}},
        {8, {0x01,0x10,0x29,0x52,0x5D,0x68,0x6D,0xF6}, {{0x7FFF,0x329F,0x001F,0},{0x7FFF,0x3FE6,0x0200,0},{0x7FFF,0x7EAC,0x40C0,0}}},
    };
    for (unsigned int t = 0; t < sizeof tab / sizeof tab[0]; t++)
        for (int k = 0; k < tab[t].n; k++)
            if (tab[t].h[k] == hash) { memcpy(out, tab[t].p, sizeof tab[t].p); return; }
    memcpy(out, grey, sizeof grey);
}

/* Palette modes: 0 classic DMG green, 1 Pocket grey, 2 auto GBC-style by
   title, 3.. the 12 GBC boot-combo palettes. */
#define DMG_PALETTE_MODES (3 + GBC_PALETTE_COUNT)
int gbp_dmg_palette_count(void) { return DMG_PALETTE_MODES; }
const char *gbp_dmg_palette_name(int mode)
{
    if (mode == 0) return "Classic green (original Game Boy)";
    if (mode == 1) return "Grey (Game Boy Pocket)";
    if (mode == 2) return "Auto colour by title (like a Game Boy Color)";
    if (mode >= 3 && mode < DMG_PALETTE_MODES) return g_gbc_combo_names[mode - 3];
    return "?";
}

static void dmg_apply_palette(void)
{
    static const unsigned int green[4] = { 0xff9bbc0f, 0xff8bac0f, 0xff306230, 0xff0f380f };
    static const unsigned int pocket[4] = { 0xffe0e0d0, 0xffa8a8a0, 0xff606058, 0xff202018 };
    int m = g_dmg_palette_mode;
    if (m == 0 || m == 1) {
        for (int l = 0; l < 3; l++) for (int s = 0; s < 4; s++) g_dmg_pal[l][s] = (m ? pocket : green)[s];
    } else if (m == 2) {
        uint16_t p[3][4];
        auto_palette(g_colour_hash, p);
        for (int l = 0; l < 3; l++) for (int s = 0; s < 4; s++) g_dmg_pal[l][s] = rgb555_argb(p[l][s]);
    } else {
        const uint32_t *p = &GBC_PALETTES[(m - 3) * 12];
        for (int l = 0; l < 3; l++) for (int s = 0; s < 4; s++) g_dmg_pal[l][s] = 0xff000000u | p[l * 4 + s];
    }
}

void gbp_set_dmg_palette(int mode)
{
    if (mode < 0 || mode >= DMG_PALETTE_MODES) mode = 2;
    g_dmg_palette_mode = mode;
    dmg_apply_palette();
}

void gbp_set_cgb_colors(int mode)
{
    g_cgb_color_mode = mode ? 1 : 0;
    if (g_cgb_lut_mode != g_cgb_color_mode) cgb_build_lut(g_cgb_color_mode);
}

static void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    if (line >= GB_H) return;
    unsigned int *dst = &g_argb[line * GB_W];
    if (gb->cgb.cgbMode) {
        for (int x = 0; x < GB_W; x++) {
            unsigned int idx = pixels[x] & 0x3f;
            const uint8_t *pal = idx < 0x20 ? &gb->cgb.BGPalette[idx * 2] : &gb->cgb.OAMPalette[(idx - 0x20) * 2];
            dst[x] = g_cgb_lut[((unsigned int)pal[1] << 8 | pal[0]) & 0x7fff];
        }
    } else {
        for (int x = 0; x < GB_W; x++) {
            unsigned int layer = (pixels[x] >> 4) & 3;
            if (layer > 2) layer = 2;
            dst[x] = g_dmg_pal[layer][pixels[x] & 3];
        }
    }
}

/* ================= ROM / cart RAM callbacks ================= */
static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return addr < g_rom_size ? g_rom[addr] : 0xff;
}
static uint16_t rom_read16(struct gb_s *gb, const uint_fast32_t addr)
{
    return (uint16_t)(rom_read(gb, addr) | (rom_read(gb, addr + 1) << 8));
}
static uint32_t rom_read32(struct gb_s *gb, const uint_fast32_t addr)
{
    return (uint32_t)rom_read(gb, addr) | ((uint32_t)rom_read(gb, addr + 1) << 8) |
           ((uint32_t)rom_read(gb, addr + 2) << 16) | ((uint32_t)rom_read(gb, addr + 3) << 24);
}
static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return (g_cart_ram && addr < g_cart_ram_size) ? g_cart_ram[addr] : 0xff;
}
static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    (void)gb;
    if (!g_cart_ram || addr >= g_cart_ram_size) return;
    if (g_cart_ram[addr] != val) {
        g_cart_ram[addr] = val;
        g_cart_dirty = 1;
        g_cart_dirty_frame = g_frames;
    }
}

/* The core says returning from gb_error is undefined (it would abort the
   whole app); unwind to gbp_run_frame instead and report the fault. */
static void core_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    static const char *names[] = { "unknown error", "invalid opcode", "invalid read", "invalid write", "halt forever" };
    snprintf(g_error, sizeof g_error, "%s at 0x%04x (pc 0x%04x)",
             (unsigned)err < 5 ? names[err] : "error", addr, gb->cpu_reg.pc.reg);
    if (g_in_frame) longjmp(g_jmp, 1);
    abort();
}

/* ================= audio ================= */
static struct minigb_apu_ctx g_apu;
#define RING_FRAMES 16384                 /* stereo frames, power of two */
#define RING_MASK   (RING_FRAMES - 1)
static short g_ring[RING_FRAMES * 2];
static volatile unsigned int g_wr, g_rd;  /* producer / consumer, in frames */
static audio_out_h g_audio;
static int g_audio_ok, g_audio_stereo;
static int g_audio_create_rc = -999;
static volatile unsigned int g_audio_callbacks, g_audio_written_bytes;
static volatile unsigned int g_audio_underruns, g_audio_overruns;
static volatile int g_audio_last_write_rc = -999;
static unsigned int g_audio_frames, g_audio_core_samples, g_audio_generated;
static unsigned int g_audio_resample_phase, g_audio_next_report;
static unsigned int g_audio_rate = 44100;
static volatile unsigned int g_frame_period_ns = 16742706u;  /* 70224 / 4194304 s */
#define AUDIO_TARGET_FILL 2048            /* ~46 ms of queued frames */
#define AUDIO_LOW_FILL     384
static volatile int g_mute, g_volume = 100;
static int g_pull_last_l, g_pull_last_r;
static int g_dc_xl, g_dc_yl, g_dc_xr, g_dc_yr;
/* Emulated-frame APU buffer and sub-frame sync. */
#define APU_FRAME_MAX 1024
static audio_sample_t g_apu_buf[APU_FRAME_MAX * 2];
static unsigned int g_apu_target;          /* samples this emulated frame */
static unsigned int g_apu_done;            /* samples rendered so far */
static unsigned long long g_apu_acc;       /* fractional sample accumulator */
static unsigned int g_apu_last_pos;
static volatile int g_channel_mask;        /* bit n set: channel n omitted */

void gbp_audio_set_mute(int m)   { g_mute = m ? 1 : 0; }
void gbp_audio_set_volume(int v) { if (v < 0) v = 0; if (v > 100) v = 100; g_volume = v; }
int  gbp_audio_get_volume(void)  { return g_volume; }
int  gbp_audio_is_muted(void)    { return g_mute; }
void gbp_audio_set_channel_filter(int f)   /* 0 all, 1..4 omit that channel */
{
    g_channel_mask = (f >= 1 && f <= 4) ? (1 << (f - 1)) : 0;
}
void gbp_set_frame_period_ns(unsigned int ns)
{
    if (ns < 10000000U) ns = 10000000U;
    if (ns > 30000000U) ns = 30000000U;
    g_frame_period_ns = ns;
}

/* Position of the CPU inside the current emulated frame, in 4.19 MHz
   cycles. A frame ends when LY reaches 144 (start of VBlank) or, with the
   LCD off, when lcd_off_count wraps. LCD timing counts real time even in
   CGB double-speed mode. Clamped to be monotonic within the frame. */
static unsigned int frame_cycle_pos(void)
{
    unsigned int pos;
    if (g_gb.hram_io[IO_LCDC] & LCDC_ENABLE) {
        unsigned int ly = g_gb.hram_io[IO_LY];
        pos = ((ly + LCD_VERT_LINES - LCD_HEIGHT) % LCD_VERT_LINES) * LCD_LINE_CYCLES + g_gb.counter.lcd_count;
    } else {
        pos = (unsigned int)g_gb.counter.lcd_off_count;
    }
    if (pos > FRAME_CYCLES) pos = FRAME_CYCLES;
    if (pos < g_apu_last_pos) pos = g_apu_last_pos;
    g_apu_last_pos = pos;
    return pos;
}

static void apu_render_to(unsigned int target)
{
    if (target > g_apu_target) target = g_apu_target;
    if (target <= g_apu_done) return;
    minigb_apu_render(&g_apu, &g_apu_buf[g_apu_done * 2], target - g_apu_done);
    g_apu_done = target;
}

static void apu_catch_up(void)
{
    if (!g_in_frame) return;
    unsigned int pos = frame_cycle_pos();
    apu_render_to((unsigned int)(((unsigned long long)pos * g_apu_target) / FRAME_CYCLES));
}

uint8_t audio_read(const uint16_t addr)
{
    apu_catch_up();
    return minigb_apu_audio_read(&g_apu, addr);
}

void audio_write(const uint16_t addr, const uint8_t val)
{
    apu_catch_up();
    minigb_apu_audio_write(&g_apu, addr, val);
}

/* Output frames for this emulated frame: its exact share of the stream
   (frame period x rate, fractional remainder carried), corrected by the ring
   fill so the TV's audio clock and the frame clock cannot drift apart: at
   most 2 frames (~0.3 %) normally, n/16 only when nearly empty or full. */
static unsigned int audio_frame_output_count(void)
{
    unsigned long long scaled = (unsigned long long)g_frame_period_ns * g_audio_rate + g_audio_resample_phase;
    int n = (int)(scaled / 1000000000ULL);
    g_audio_resample_phase = (unsigned int)(scaled % 1000000000ULL);
    int fill = (int)(g_wr - g_rd);
    if (fill < AUDIO_LOW_FILL) n += n / 16;
    else if (fill > 3 * AUDIO_TARGET_FILL) n -= n / 16;
    else {
        int adj = (AUDIO_TARGET_FILL - fill) / 512;
        if (adj > 2) adj = 2;
        if (adj < -2) adj = -2;
        n += adj;
    }
    if (n < 1) n = 1;
    return (unsigned int)n;
}

/* Async pull callback (audio thread): serve nbytes from the ring; on
   underrun fade the last value toward silence instead of jumping. */
static void audio_pull(audio_out_h handle, unsigned int nbytes, void *ud)
{
    (void)ud;
    g_audio_callbacks++;
    unsigned char tmp[4096];
    unsigned int frame_bytes = g_audio_stereo ? 4 : 2;
    while (nbytes) {
        unsigned int chunk = nbytes > sizeof tmp ? sizeof tmp : nbytes;
        unsigned int count = chunk / frame_bytes;
        unsigned int rd = g_rd, wr = g_wr;
        __sync_synchronize();
        for (unsigned int i = 0; i < count; i++) {
            int l, r;
            if (rd != wr) {
                l = g_ring[(rd & RING_MASK) * 2]; r = g_ring[(rd & RING_MASK) * 2 + 1]; rd++;
                g_pull_last_l = l; g_pull_last_r = r;
            } else {
                g_audio_underruns++;
                g_pull_last_l -= g_pull_last_l >> 4; g_pull_last_r -= g_pull_last_r >> 4;
                l = g_pull_last_l; r = g_pull_last_r;
            }
            if (g_mute) l = r = 0;
            else if (g_volume != 100) { l = l * g_volume / 100; r = r * g_volume / 100; }
            if (g_audio_stereo) {
                tmp[i * 4]     = (unsigned char)(l & 0xff);
                tmp[i * 4 + 1] = (unsigned char)((l >> 8) & 0xff);
                tmp[i * 4 + 2] = (unsigned char)(r & 0xff);
                tmp[i * 4 + 3] = (unsigned char)((r >> 8) & 0xff);
            } else {
                int m = (l + r) / 2;
                tmp[i * 2]     = (unsigned char)(m & 0xff);
                tmp[i * 2 + 1] = (unsigned char)((m >> 8) & 0xff);
            }
        }
        __sync_synchronize();
        g_rd = rd;
        for (unsigned int i = count * frame_bytes; i < chunk; i++) tmp[i] = 0;
        g_audio_last_write_rc = audio_out_write(handle, tmp, chunk);
        g_audio_written_bytes += chunk;
        nbytes -= chunk;
    }
}

static void audio_open(void)
{
    char line[200];
    g_audio = 0; g_audio_ok = 0; g_audio_stereo = 1;
    g_audio_callbacks = g_audio_written_bytes = g_audio_underruns = g_audio_overruns = 0;
    g_audio_frames = g_audio_core_samples = g_audio_generated = 0;
    g_audio_resample_phase = 0; g_audio_next_report = 120;
    g_dc_xl = g_dc_yl = g_dc_xr = g_dc_yr = 0;
    g_pull_last_l = g_pull_last_r = 0;
    /* Prequeue the target amount of silence (TizNES 1.0.24 lesson): steady
       state starts at the fill the per-frame correction aims for. */
    memset(g_ring, 0, sizeof g_ring);
    g_rd = 0; g_wr = AUDIO_TARGET_FILL;
    g_audio_create_rc = audio_out_create_new((int)g_audio_rate, AUDIO_CHANNEL_STEREO, AUDIO_SAMPLE_TYPE_S16LE, &g_audio);
    if (g_audio_create_rc != 0 || !g_audio) {
        snprintf(line, sizeof line, "audio-open stereo create_rc=%d; trying mono", g_audio_create_rc);
        gbp_diag(line);
        g_audio = 0; g_audio_stereo = 0;
        g_audio_create_rc = audio_out_create_new((int)g_audio_rate, AUDIO_CHANNEL_MONO, AUDIO_SAMPLE_TYPE_S16LE, &g_audio);
    }
    if (g_audio_create_rc != 0 || !g_audio) {
        snprintf(line, sizeof line, "audio-open create_rc=%d (no audio)", g_audio_create_rc);
        gbp_diag(line); g_audio = 0; return;
    }
    int rc = audio_out_set_stream_cb(g_audio, audio_pull, 0);
    if (rc == 0) rc = audio_out_prepare(g_audio);
    if (rc != 0) {
        snprintf(line, sizeof line, "audio-open setup_rc=%d", rc);
        gbp_diag(line); audio_out_destroy(g_audio); g_audio = 0; return;
    }
    g_audio_ok = 1;
    snprintf(line, sizeof line, "audio-open ok rate=%u channels=%s type=s16 prequeue=%d",
             g_audio_rate, g_audio_stereo ? "stereo" : "mono", AUDIO_TARGET_FILL);
    gbp_diag(line);
}

static void audio_close(void)
{
    if (g_audio) { audio_out_unprepare(g_audio); audio_out_destroy(g_audio); g_audio = 0; }
    g_audio_ok = 0;
}

static int clamp16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

/* Resample the finished emulated frame, DC-block it (one-pole high-pass,
   ~27 Hz, close to the Game Boy's own output capacitor) and queue it. */
static void audio_frame_done(void)
{
    apu_render_to(g_apu_target);
    if (!g_audio_ok) return;
    unsigned int in = g_apu_target, out = audio_frame_output_count();
    unsigned int wr = g_wr, rd = g_rd;
    g_audio_frames++;
    g_audio_core_samples += in;
    if (in) for (unsigned int i = 0; i < out; i++) {
        unsigned int position = i * in;
        unsigned int index = position / out, fraction = position % out;
        unsigned int next = index + 1 < in ? index + 1 : index;
        int l0 = g_apu_buf[index * 2], l1 = g_apu_buf[next * 2];
        int r0 = g_apu_buf[index * 2 + 1], r1 = g_apu_buf[next * 2 + 1];
        int l = l0 + ((l1 - l0) * (int)fraction) / (int)out;
        int r = r0 + ((r1 - r0) * (int)fraction) / (int)out;
        /* State in Q8: with plain integers y - (y >> 8) cannot decay
           below 256, leaving up to 255 LSB of DC after every sound. */
        g_dc_yl = ((l - g_dc_xl) << 8) + g_dc_yl - (g_dc_yl >> 8); g_dc_xl = l;
        g_dc_yr = ((r - g_dc_xr) << 8) + g_dc_yr - (g_dc_yr >> 8); g_dc_xr = r;
        if (wr - rd >= RING_MASK) { g_audio_overruns++; break; }
        g_ring[(wr & RING_MASK) * 2] = (short)clamp16(g_dc_yl >> 8);
        g_ring[(wr & RING_MASK) * 2 + 1] = (short)clamp16(g_dc_yr >> 8);
        wr++;
    }
    __sync_synchronize();
    g_wr = wr;
    g_audio_generated += out;
    if (g_audio_frames >= g_audio_next_report) {
        char line[256];
        snprintf(line, sizeof line,
            "audio-runtime frames=%u core_samples=%u generated=%u callbacks=%u bytes=%u last_write_rc=%d fill=%u underrun=%u overrun=%u period_ns=%u rate=%u",
            g_audio_frames, g_audio_core_samples, g_audio_generated, g_audio_callbacks, g_audio_written_bytes,
            g_audio_last_write_rc, g_wr - g_rd, g_audio_underruns, g_audio_overruns, g_frame_period_ns, g_audio_rate);
        gbp_diag(line);
        g_audio_next_report = g_audio_frames + 3600;
    }
}

/* ================= saves ================= */
static int write_file_atomic(const char *path, const void *data, unsigned long n)
{
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long w = write(fd, data, n);
    fsync(fd);
    close(fd);
    if (w != (long)n) { unlink(tmp); return -1; }
    return rename(tmp, path);
}

static void rtc_save(void)
{
    if (!g_has_rtc) return;
    unsigned char buf[16];
    long long now = (long long)time(0);
    memcpy(buf, g_gb.rtc_real.bytes, 5);
    for (int i = 0; i < 8; i++) buf[5 + i] = (unsigned char)(now >> (8 * i));
    buf[13] = 'R'; buf[14] = 'T'; buf[15] = '1';
    write_file_atomic(g_rtc_path, buf, sizeof buf);
}

/* Restore the cartridge clock and advance it by the time spent away (a
   halted clock, bit 6 of the high register, does not advance). Without a
   saved clock, start from the TV's local time. */
static void rtc_load(void)
{
    unsigned char buf[16];
    int fd = open(g_rtc_path, O_RDONLY), ok = 0;
    if (fd >= 0) {
        ok = read(fd, buf, sizeof buf) == (long)sizeof buf && buf[13] == 'R' && buf[14] == 'T' && buf[15] == '1';
        close(fd);
    }
    if (!ok) {
        time_t now = time(0);
        struct tm tm;
        if (localtime_r(&now, &tm)) gb_set_rtc(&g_gb, &tm);
        return;
    }
    long long saved = 0, now = (long long)time(0);
    for (int i = 0; i < 8; i++) saved |= (long long)buf[5 + i] << (8 * i);
    unsigned char high = buf[4];
    long long total = buf[0] + 60LL * buf[1] + 3600LL * buf[2] + 86400LL * (buf[3] | ((high & 1) << 8));
    if (!(high & 0x40) && now > saved) total += now - saved;
    long long days = total / 86400;
    if (days > 511) { high |= 0x80; days %= 512; }
    g_gb.rtc_real.bytes[0] = (uint8_t)(total % 60);
    g_gb.rtc_real.bytes[1] = (uint8_t)((total / 60) % 60);
    g_gb.rtc_real.bytes[2] = (uint8_t)((total / 3600) % 24);
    g_gb.rtc_real.bytes[3] = (uint8_t)(days & 0xff);
    g_gb.rtc_real.bytes[4] = (uint8_t)((high & 0xc0) | ((days >> 8) & 1));
}

int gbp_save_now(void)
{
    int rc = 0;
    if (!g_loaded) return 0;
    if (g_cart_ram && g_cart_ram_size && g_cart_dirty) {
        rc = write_file_atomic(g_save_path, g_cart_ram, (unsigned long)g_cart_ram_size);
        char line[700];
        snprintf(line, sizeof line, "save %s bytes=%lu rc=%d", g_save_path, (unsigned long)g_cart_ram_size, rc);
        gbp_diag(line);
        if (rc == 0) g_cart_dirty = 0;
    }
    rtc_save();
    return rc;
}

/* ================= bridge ================= */
unsigned int *gbp_framebuffer(void) { return g_argb; }
int  gbp_width(void)  { return GB_W; }
int  gbp_height(void) { return GB_H; }
int  gbp_is_cgb(void) { return g_loaded && g_gb.cgb.cgbMode; }
const char *gbp_last_error(void) { return g_error; }
int  gbp_last_error_is_io(void) { return g_error_io; }
const char *gbp_save_key(void) { return g_save_key; }
const char *gbp_save_path(void) { return g_save_path; }

/* Frontend pad bits use the TizNES layout (A 1, B 2, Select 4, Start 8,
   Up 0x10, Down 0x20, Left 0x40, Right 0x80); the core wants its own. */
void gbp_set_pad(unsigned int nes_bits)
{
    unsigned int b = 0;
    if (nes_bits & 0x01) b |= JOYPAD_A;
    if (nes_bits & 0x02) b |= JOYPAD_B;
    if (nes_bits & 0x04) b |= JOYPAD_SELECT;
    if (nes_bits & 0x08) b |= JOYPAD_START;
    if (nes_bits & 0x10) b |= JOYPAD_UP;
    if (nes_bits & 0x20) b |= JOYPAD_DOWN;
    if (nes_bits & 0x40) b |= JOYPAD_LEFT;
    if (nes_bits & 0x80) b |= JOYPAD_RIGHT;
    g_pad = b;
}

void gbp_init(void)
{
    int fd = open(DIAG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "app-start\n", 10); close(fd); }
    mkdir(SAVE_DIR, 0755);
    cgb_build_lut(g_cgb_color_mode);
    dmg_apply_palette();
    memset(g_argb, 0, sizeof g_argb);
}

static void release_rom(void)
{
    if (g_rom) { free(g_rom); g_rom = 0; }
    if (g_cart_ram) { free(g_cart_ram); g_cart_ram = 0; }
    g_rom_size = 0; g_cart_ram_size = 0; g_loaded = 0;
}

static const char *errno_text(int e)
{
    switch (e) {
    case E_NOENT: return "file not found";
    case E_IO:    return "I/O error";
    case E_NXIO: case E_NODEV: return "device not present";
    case E_STALE: return "stale file handle";
    default: return "error";
    }
}

/* Save key: header title (A-Z 0-9, others '_', at most 16) + '-' + FNV-1a
   64 of the full ROM file. */
static void make_save_key(void)
{
    char t[17]; int k = 0, last_us = 1;
    for (unsigned int a = 0x134; a < 0x143 && k < 16; a++) {
        uint8_t c = rom_read(0, a);
        if (!c) break;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) { t[k++] = (char)c; last_us = 0; }
        else if (!last_us) { t[k++] = '_'; last_us = 1; }
    }
    while (k && t[k - 1] == '_') k--;
    t[k] = 0;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned long i = 0; i < g_rom_size; i++) { h ^= g_rom[i]; h *= 0x100000001b3ULL; }
    snprintf(g_save_key, sizeof g_save_key, "%s-%08x%08x", k ? t : "ROM", (unsigned)(h >> 32), (unsigned)h);
}

/* path: where to load the ROM from (internal or a USB drive).
   base: its file name, for logs. 0 on success. */
int gbp_load_rom(const char *path, const char *base)
{
    char line[700];
    g_error[0] = 0; g_error_io = 0;
    release_rom();
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) {
        int e = *__errno_location();
        snprintf(g_error, sizeof g_error, "cannot open the file (%s, errno %d)", errno_text(e), e);
        g_error_io = 1; return -1;
    }
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    g_rom = (uint8_t *)malloc(MAX_ROM_BYTES);
    if (!g_rom) { close(fd); snprintf(g_error, sizeof g_error, "out of memory"); return -1; }
    unsigned long total = 0;
    int read_errno = 0;
    for (;;) {
        long n = read(fd, g_rom + total, MAX_ROM_BYTES - total);
        if (n < 0 && *__errno_location() == E_INTR) continue;
        if (n < 0) { read_errno = *__errno_location(); break; }
        if (n == 0) break;
        total += (unsigned long)n;
        if (total >= MAX_ROM_BYTES) break;
    }
    close(fd);
    g_rom_size = total;
    unsigned long expect = size > 0 ? ((unsigned long)size < MAX_ROM_BYTES ? (unsigned long)size : MAX_ROM_BYTES) : 0;
    if (read_errno || total < expect) {
        if (read_errno) snprintf(g_error, sizeof g_error, "read failed after %lu bytes (%s, errno %d)", total, errno_text(read_errno), read_errno);
        else snprintf(g_error, sizeof g_error, "file ended early (%lu of %lu bytes)", total, expect);
        g_error_io = 1;
        release_rom(); return -1;
    }
    if (total < 0x8000 && total < 0x150) {
        snprintf(g_error, sizeof g_error, "file too small for a Game Boy ROM (%lu bytes)", total);
        release_rom(); return -1;
    }
    memset(&g_gb, 0, sizeof g_gb);
    enum gb_init_error_e err = gb_init(&g_gb, rom_read, rom_read16, rom_read32,
                                       cart_ram_read, cart_ram_write, core_error, 0);
    if (err != GB_INIT_NO_ERROR) {
        snprintf(g_error, sizeof g_error, err == GB_INIT_INVALID_CHECKSUM ? "ROM header checksum is invalid" :
                 err == GB_INIT_CARTRIDGE_UNSUPPORTED ? "cartridge type 0x%02x is not supported" : "unknown load error %d",
                 err == GB_INIT_CARTRIDGE_UNSUPPORTED ? rom_read(0, 0x147) : (int)err);
        release_rom(); return -1;
    }
    gb_init_lcd(&g_gb, lcd_draw_line);
    size_t ram = 0;
    gb_get_save_size_s(&g_gb, &ram);
    make_save_key();
    snprintf(g_save_path, sizeof g_save_path, "%s/%s.sav", SAVE_DIR, g_save_key);
    snprintf(g_rtc_path, sizeof g_rtc_path, "%s/%s.rtc", SAVE_DIR, g_save_key);
    snprintf(line, sizeof line, "save-key %s rom=%.500s", g_save_key, path);
    gbp_diag(line);
    g_cart_ram_size = ram;
    g_cart_dirty = 0;
    if (ram) {
        g_cart_ram = (uint8_t *)malloc(ram);
        if (!g_cart_ram) { snprintf(g_error, sizeof g_error, "out of memory for cart RAM"); release_rom(); return -1; }
        memset(g_cart_ram, 0xff, ram);
        int sfd = open(g_save_path, O_RDONLY);
        if (sfd >= 0) {
            long n = read(sfd, g_cart_ram, ram);
            close(sfd);
            snprintf(line, sizeof line, "save-load %s bytes=%ld of %lu", g_save_path, n, (unsigned long)ram);
            gbp_diag(line);
        } else {
            /* No internal save yet: import "<rom without extension>.sav"
               from beside the ROM, read-only. Marked dirty so the first
               automatic save copies it to internal storage. */
            char side[600];
            int n = snprintf(side, sizeof side, "%s", path), dot = -1;
            for (int i = 0; i < n && i < (int)sizeof side; i++) { if (side[i] == '/') dot = -1; else if (side[i] == '.') dot = i; }
            if (dot > 0 && n < (int)sizeof side - 4) {
                snprintf(side + dot, sizeof side - (unsigned long)dot, ".sav");
                int ifd = open(side, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
                if (ifd >= 0) {
                    long got = read(ifd, g_cart_ram, ram);
                    close(ifd);
                    if (got > 0) { g_cart_dirty = 1; g_cart_dirty_frame = 0; }
                    else memset(g_cart_ram, 0xff, ram);
                    snprintf(line, sizeof line, "save-import %.500s bytes=%ld of %lu", side, got, (unsigned long)ram);
                    gbp_diag(line);
                }
            }
        }
    }
    uint8_t type = rom_read(0, 0x147);
    g_has_rtc = (type == 0x0f || type == 0x10);
    if (g_has_rtc) rtc_load();
    g_colour_hash = gb_colour_hash(&g_gb);
    dmg_apply_palette();
    memset(&g_apu, 0, sizeof g_apu);
    minigb_apu_audio_init(&g_apu);
    g_apu_acc = 0; g_apu_done = g_apu_target = 0; g_apu_last_pos = 0;
    g_frames = 0;
    g_loaded = 1;
    char title[20];
    gb_get_rom_name(&g_gb, title);
    snprintf(line, sizeof line, "rom-load ok file=%.200s title=%s cgb_flag=0x%02x mode=%s mbc=%d type=0x%02x rom=%lu ram=%lu rtc=%d hash=0x%02x",
             base ? base : "", title, rom_read(0, 0x143), g_gb.cgb.cgbMode ? "CGB" : "DMG", g_gb.mbc, type,
             g_rom_size, (unsigned long)ram, g_has_rtc, g_colour_hash);
    gbp_diag(line);
    audio_open();
    return 0;
}

/* Run one emulated frame. Returns 0, or -1 after a core fault (the game is
   stopped; gbp_last_error() says why). */
int gbp_run_frame(void)
{
    if (!g_loaded) return -1;
    g_gb.direct.joypad = (uint8_t)~g_pad;  /* active low */
    g_apu_acc += (unsigned long long)AUDIO_SAMPLE_RATE * FRAME_CYCLES;
    g_apu_target = (unsigned int)(g_apu_acc / GB_CLOCK);
    g_apu_acc %= GB_CLOCK;
    if (g_apu_target > APU_FRAME_MAX) g_apu_target = APU_FRAME_MAX;
    g_apu_done = 0; g_apu_last_pos = 0;
    g_apu.mute_mask = (uint8_t)g_channel_mask;
    g_in_frame = 1;
    if (_setjmp(g_jmp)) {
        g_in_frame = 0;
        char line[220]; snprintf(line, sizeof line, "core-fault %s", g_error);
        gbp_diag(line);
        gbp_save_now();
        return -1;
    }
#ifdef TIZGB_DUALFETCH
    gb_run_frame_dualfetch(&g_gb);
#else
    gb_run_frame(&g_gb);   /* original Peanut-GB dispatch: the conservative path */
#endif
    audio_frame_done();
    g_in_frame = 0;
    g_frames++;
    /* Persist battery RAM once the game has stopped writing for ~2 s. */
    if (g_cart_dirty && g_frames - g_cart_dirty_frame > 120) gbp_save_now();
    return 0;
}

void gbp_stop(void)
{
    if (g_loaded) gbp_save_now();
    audio_close();
    release_rom();
}

/* ---- host-harness hooks (not used by the TV frontend) ---- */
struct gb_s *gbp_core(void) { return &g_gb; }
struct minigb_apu_ctx *gbp_apu(void) { return &g_apu; }
unsigned int gbp_ring_fill(void) { return g_wr - g_rd; }
unsigned int gbp_audio_underruns(void) { return g_audio_underruns; }
unsigned int gbp_audio_overruns(void) { return g_audio_overruns; }
void gbp_audio_pull_for_test(unsigned int nbytes) { audio_pull(g_audio, nbytes, 0); }
const short *gbp_ring(void) { return g_ring; }
unsigned int gbp_ring_rd(void) { return g_rd; }
