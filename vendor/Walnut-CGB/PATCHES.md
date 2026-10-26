# Local changes to vendored Walnut-CGB 1.2.4 (upstream a42c186)

`walnut_cgb.h` and `extras/cgb.h` are **unmodified** (see `UPSTREAM.sha256`).
`app/platform.c` declares `struct gb_s;` before including the header because
upstream prototypes functions taking `struct gb_s *` before defining the
struct, which is an error in C (it compiles as C++).

`minigb_apu/minigb_apu.c` / `.h` (MIT, Alex Baines / Mahyar Koshkouei) carry
the following TizGB patches, each marked `TizGB` in the source. They were made
after reviewing the APU for the same class of bug InfoNES had (TizNES 1.0.24:
unsigned envelope phase counters that silently disabled hardware fades):

1. **Sub-frame rendering API** `minigb_apu_render(ctx, stream, nframes)`.
   Upstream only rendered a whole video frame of samples at once (in the SDL
   example, from the audio thread, racing the CPU). TizGB renders up to the
   position of every APU register access inside the frame, so register
   changes keep their timing. `minigb_apu_audio_callback()` remains as a
   wrapper.
2. **Box-filtered output.** Upstream computed the weight of each waveform
   segment as `((pos - prev_pos) / freq_inc)` in unsigned integers, which is
   always 0: every output sample was the waveform at one instant (point
   sampling), aliasing badly on high notes. Segments are now weighted with
   64-bit arithmetic. The noise channel now weights the segment before each
   LFSR step with the value it had before the step.
3. **Wave output level applied once.** `wave_sample()` already shifts the
   4-bit sample for the 100/50/25 % level, and upstream then divided by
   1/2/4 again (50 % played at 25 %, 25 % at about 6 %).
4. **Frequency sweep follows hardware.** Upstream disabled channel 1 on every
   sweep tick whenever the sweep shift was 0 (hardware only disables it if
   freq + freq >> 0 overflows 2047), ticked once immediately at trigger
   (hardware waits one period), skipped the trigger-time overflow check, and
   stopped sweeping while the envelope volume was 0.
5. **Noise clock shift 14/15** stops the LFSR (hardware behaviour) instead of
   switching the channel off.
6. **Wave RAM** stays writable while the APU is powered off.
7. **`mute_mask`** (diagnostics only): a masked channel is still emulated
   (length, envelope and NR52 status keep running) but rendered into a
   scratch buffer. Used by Settings > Audio channel test.

Checked and found correct (no change): envelope counters are unsigned
accumulators compared upward (`counter > FREQ_INC_REF`), so they cannot get
stuck the way InfoNES's did; volume decrements with wrap-safe 8-bit arithmetic
and stops at 0; envelope rate 64/step Hz, length 256 Hz, sweep 128/pace Hz and
noise divisors match the hardware. The host harness verifies a hardware
envelope fade end to end (`harness/apu_test`).
