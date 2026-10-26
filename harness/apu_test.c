/*
 * apu_test.c - end-to-end audio harness: runs a test ROM through the real
 * app/platform.c (CPU -> patched APU -> resampler -> DC blocker -> ring ->
 * pull callback) while a simulated TV audio device pulls at exactly
 * 44100 Hz, captures what the TV would play, writes a WAV, and prints an RMS
 * envelope in fixed windows plus ring statistics.
 *
 * usage: apu_test <rom> <seconds> <out.wav> <window_ms> [channel_filter]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

extern void gbp_init(void);
extern int  gbp_load_rom(const char *, const char *);
extern int  gbp_run_frame(void);
extern void gbp_stop(void);
extern unsigned int gbp_ring_fill(void);
extern unsigned int gbp_audio_underruns(void);
extern unsigned int gbp_audio_overruns(void);
extern void gbp_audio_pull_for_test(unsigned int);
extern void gbp_audio_set_channel_filter(int);
extern const char *gbp_last_error(void);

static int16_t *g_cap;
static size_t g_cap_n, g_cap_cap;

typedef void *audio_out_h;
int audio_out_create_new(int r, int c, int t, audio_out_h *h) { (void)r; (void)c; (void)t; *h = (void *)1; return 0; }
int audio_out_set_stream_cb(audio_out_h h, void *cb, void *u) { (void)h; (void)cb; (void)u; return 0; }
int audio_out_prepare(audio_out_h h) { (void)h; return 0; }
int audio_out_unprepare(audio_out_h h) { (void)h; return 0; }
int audio_out_destroy(audio_out_h h) { (void)h; return 0; }
int audio_out_write(audio_out_h h, void *b, unsigned int n)
{
    (void)h;
    if (g_cap_n + n / 2 > g_cap_cap) {
        g_cap_cap = (g_cap_n + n / 2) * 2 + 65536;
        g_cap = realloc(g_cap, g_cap_cap * sizeof *g_cap);
    }
    memcpy(g_cap + g_cap_n, b, n);
    g_cap_n += n / 2;
    return (int)n;
}

static void wav(const char *path, const int16_t *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)(n * 2), riff = 36 + data, rate = 44100, br = rate * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(s, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    double seconds = atof(argv[2]);
    int window_ms = atoi(argv[4]);
    gbp_init();
    if (argc > 5) gbp_audio_set_channel_filter(atoi(argv[5]));
    if (gbp_load_rom(argv[1], "apu") != 0) { printf("LOAD-FAIL %s\n", gbp_last_error()); return 1; }
    /* The TV pulls at 44.1 kHz in its own blocks; emulate 4096-byte pulls
       paced by an exact sample clock against the 59.7275 Hz frame clock. */
    double frame_s = 70224.0 / 4194304.0, t = 0, pulled_s = 0;
    unsigned int max_fill = 0, min_fill = ~0u;
    int frames = (int)(seconds / frame_s);
    for (int f = 0; f < frames; f++) {
        if (gbp_run_frame() != 0) { printf("FAULT %s\n", gbp_last_error()); return 1; }
        t += frame_s;
        while (pulled_s + 1024.0 / 44100.0 <= t) { gbp_audio_pull_for_test(4096); pulled_s += 1024.0 / 44100.0; }
        unsigned int fill = gbp_ring_fill();
        if (f > 60) { if (fill > max_fill) max_fill = fill; if (fill < min_fill) min_fill = fill; }
    }
    size_t nframes = g_cap_n / 2;
    /* Skip the 2048-frame silence prequeue so t=0 is the emulated start. */
    size_t skip = 2048 < nframes ? 2048 : 0;
    wav(argv[3], g_cap + skip * 2, (nframes - skip) * 2);
    int win = 44100 * window_ms / 1000;
    printf("rom=%s frames=%d captured=%zu underruns=%u overruns=%u fill_after_1s=[%u..%u]\n",
           argv[1], frames, nframes, gbp_audio_underruns(), gbp_audio_overruns(), min_fill, max_fill);
    printf("rms_l (window %d ms):", window_ms);
    for (size_t w = skip; w + win <= nframes; w += win) {
        double acc = 0;
        for (int i = 0; i < win; i++) { double v = g_cap[(w + i) * 2]; acc += v * v; }
        printf(" %.0f", sqrt(acc / win));
    }
    printf("\n");
    /* zero-crossing rate per window (pitch tracking for the sweep test) */
    printf("zc_per_window:");
    for (size_t w = skip; w + win <= nframes; w += win) {
        int zc = 0;
        for (int i = 1; i < win; i++) if ((g_cap[(w + i - 1) * 2] < 0) != (g_cap[(w + i) * 2] < 0)) zc++;
        printf(" %d", zc);
    }
    printf("\n");
    gbp_stop();
    return 0;
}
