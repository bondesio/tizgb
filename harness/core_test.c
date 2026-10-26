/*
 * core_test.c - host harness for the real app/platform.c (compiled
 * freestanding with app/include, exactly as on the TV) against free test
 * ROMs: Blargg cpu_instrs / instr_timing (serial output), dmg-acid2 and
 * cgb-acid2 (framebuffer dumped as PPM for compare_acid.py).
 *
 * usage: core_test serial <rom> <frames>
 *        core_test frame  <rom> <frames> <out.ppm> <dmg_palette> <cgb_colors>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct gb_s;
extern void gbp_init(void);
extern int  gbp_load_rom(const char *, const char *);
extern int  gbp_run_frame(void);
extern void gbp_stop(void);
extern unsigned int *gbp_framebuffer(void);
extern void gbp_set_dmg_palette(int);
extern void gbp_set_cgb_colors(int);
extern int  gbp_is_cgb(void);
extern const char *gbp_last_error(void);
extern struct gb_s *gbp_core(void);
extern void gb_init_serial(struct gb_s *, void (*)(struct gb_s *, const uint8_t), void *);

/* capi-media-audio-io stand-ins: audio is opened but never pulled here. */
typedef void *audio_out_h;
int audio_out_create_new(int r, int c, int t, audio_out_h *h) { (void)r; (void)c; (void)t; *h = (void *)1; return 0; }
int audio_out_set_stream_cb(audio_out_h h, void *cb, void *u) { (void)h; (void)cb; (void)u; return 0; }
int audio_out_prepare(audio_out_h h) { (void)h; return 0; }
int audio_out_unprepare(audio_out_h h) { (void)h; return 0; }
int audio_out_destroy(audio_out_h h) { (void)h; return 0; }
int audio_out_write(audio_out_h h, void *b, unsigned int n) { (void)h; (void)b; return (int)n; }

static char g_serial[8192];
static size_t g_serial_n;
static void serial_tx(struct gb_s *gb, const uint8_t tx)
{
    (void)gb;
    if (g_serial_n < sizeof g_serial - 1) g_serial[g_serial_n++] = tx < 32 ? ' ' : (char)tx;
    g_serial[g_serial_n] = 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
    gbp_init();
    if (argc >= 7) { gbp_set_dmg_palette(atoi(argv[5])); gbp_set_cgb_colors(atoi(argv[6])); }
    if (gbp_load_rom(argv[2], "harness") != 0) { printf("LOAD-FAIL %s\n", gbp_last_error()); return 1; }
    int frames = atoi(argv[3]);
    if (!strcmp(argv[1], "serial")) {
        gb_init_serial(gbp_core(), serial_tx, NULL);
        for (int f = 0; f < frames; f++) {
            if (gbp_run_frame() != 0) { printf("FAULT %s\n", gbp_last_error()); return 1; }
            if (strstr(g_serial, "Passed") || strstr(g_serial, "Failed")) {
                /* let the final line finish */
                for (int k = 0; k < 30; k++) gbp_run_frame();
                break;
            }
        }
        printf("frames-run serial: %s\n", g_serial);
        int ok = strstr(g_serial, "Passed") && !strstr(g_serial, "Failed");
        printf("%s\n", ok ? "RESULT PASS" : "RESULT FAIL");
        gbp_stop();
        return ok ? 0 : 1;
    }
    for (int f = 0; f < frames; f++)
        if (gbp_run_frame() != 0) { printf("FAULT %s\n", gbp_last_error()); return 1; }
    FILE *fp = fopen(argv[4], "wb");
    if (!fp) return 1;
    fprintf(fp, "P6\n160 144\n255\n");
    unsigned int *fb = gbp_framebuffer();
    for (int i = 0; i < 160 * 144; i++) {
        unsigned char px[3] = { (unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i] };
        fwrite(px, 1, 3, fp);
    }
    fclose(fp);
    printf("mode=%s wrote %s\n", gbp_is_cgb() ? "CGB" : "DMG", argv[4]);
    gbp_stop();
    return 0;
}
