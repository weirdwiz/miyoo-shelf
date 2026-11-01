// Tests for the Miyoo audio backend (src/audio_mm.c): the click WAV conversion, mixing a
// game source with clicks, swapping sources, the idle shutdown, and dump pacing. The
// backend picks SHELF_AUDIO_DUMP whenever libmi_ao is absent, so this runs on the Mac host
// and in the VM, both of which have no vendor libraries.
// usage: test-audio-mm [work-dir]
#include "../src/platform.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RATE PLATFORM_AUDIO_RATE
#define TWO_PI 6.283185307179586477
#define FULL_SCALE 32768.0

// audio_mm.c asks platform_volume() for the level when it enables MI_AO. The host has no
// MI libraries, so this only has to exist for the link.
int platform_volume(void) { return 12; }

static char work[512];
static const char *self;
static int check_count;

static void fail_at(int line, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "FAIL line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        check_count++;                                                         \
        if (!(cond)) fail_at(__LINE__, __VA_ARGS__);                           \
    } while (0)

// --- helpers ------------------------------------------------------------------

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_until(double t)
{
    for (double d = t - now(); d > 0; d = t - now()) {
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = (long)(d * 1e9) + 1;
        if (ts.tv_nsec > 999999999) ts.tv_nsec = 999999999;
        nanosleep(&ts, NULL);
    }
}

static void sleep_for(double seconds)
{
    sleep_until(now() + seconds);
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;

    if (!d) {
        remove(path);
        return;
    }
    while ((e = readdir(d))) {
        char child[768]; // work[] is 512, a dirent name at most 255

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        remove(child);
    }
    closedir(d);
    rmdir(path);
}

static size_t dump_bytes(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

// Reads a dump as interleaved stereo frames.
static int16_t *read_dump(const char *path, size_t *frames)
{
    FILE *f = fopen(path, "rb");
    long size = 0;
    int16_t *s;

    if (!f) fail_at(__LINE__, "cannot open %s", path);
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0) fail_at(__LINE__, "cannot size %s", path);
    rewind(f);
    s = malloc((size_t)size);
    if (!s || fread(s, 1, (size_t)size, f) != (size_t)size) fail_at(__LINE__, "cannot read %s", path);
    fclose(f);
    *frames = (size_t)size / (2 * sizeof *s);
    return s;
}

// Amplitude of the component at `freq` in one channel, 0..1 of full scale.
static double tone_amp(const int16_t *s, size_t frames, int ch, double freq)
{
    double re = 0, im = 0;

    for (size_t i = 0; i < frames; i++) {
        double w = TWO_PI * freq * (double)i / RATE;

        re += (double)s[2 * i + ch] * cos(w);
        im += (double)s[2 * i + ch] * sin(w);
    }
    return 2 * sqrt(re * re + im * im) / (double)frames / FULL_SCALE;
}

static double peak(const int16_t *s, size_t frames)
{
    int v = 0;

    for (size_t i = 0; i < 2 * frames; i++) {
        int a = s[i] < 0 ? -s[i] : s[i];

        if (a > v) v = a;
    }
    return (double)v / FULL_SCALE;
}

// Frames up to and including the last one that is not near-silence. The backend writes
// silence after a click until it idles, so the dump is longer than the click.
static size_t audible_frames(const int16_t *s, size_t frames)
{
    size_t last = 0;

    for (size_t i = 0; i < frames; i++)
        if (abs(s[2 * i]) > 8 || abs(s[2 * i + 1]) > 8) last = i + 1;
    return last;
}

// --- generated WAVs -----------------------------------------------------------

static void le16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void le32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

// Writes a canonical 44-byte header, then `present_bytes` of samples. `declared_bytes` goes
// into the chunk sizes, so it can claim more data than the file holds.
static FILE *wav_start(const char *path, int bits, int channels, long rate, size_t declared_bytes)
{
    unsigned char h[44];
    FILE *f = fopen(path, "wb");

    if (!f) fail_at(__LINE__, "cannot create %s", path);
    memcpy(h, "RIFF", 4);
    le32(h + 4, (unsigned long)(36 + declared_bytes));
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    le32(h + 16, 16);
    le16(h + 20, 1); // PCM
    le16(h + 22, (unsigned)channels);
    le32(h + 24, (unsigned long)rate);
    le32(h + 28, (unsigned long)rate * channels * (bits / 8));
    le16(h + 32, (unsigned)(channels * (bits / 8)));
    le16(h + 34, (unsigned)bits);
    memcpy(h + 36, "data", 4);
    le32(h + 40, (unsigned long)declared_bytes);
    fwrite(h, 1, sizeof h, f);
    return f;
}

// A 16-bit PCM WAV at `rate` with a tone per channel, truncated before its declared length
// when `present` is smaller than `frames`.
static void wav_tone(const char *path, int channels, long rate, const double *freqs,
                     const double *amps, size_t frames, size_t present)
{
    FILE *f = wav_start(path, 16, channels, rate, frames * (size_t)channels * 2);

    for (size_t i = 0; i < present; i++)
        for (int c = 0; c < channels; c++) {
            int16_t v = (int16_t)lrint(amps[c] * 32767.0 * sin(TWO_PI * freqs[c] * (double)i / (double)rate));

            fwrite(&v, sizeof v, 1, f);
        }
    fclose(f);
}

// An 8-bit WAV: the backend must refuse it and synthesize its own click.
static void wav_8bit(const char *path, long rate, size_t frames)
{
    FILE *f = wav_start(path, 8, 1, rate, frames);

    for (size_t i = 0; i < frames; i++) {
        unsigned char v = (unsigned char)(0x80 + ((i / 8) & 1 ? 0x40 : -0x40));

        fwrite(&v, 1, 1, f);
    }
    fclose(f);
}

// RIFF/WAVE, then a fmt chunk header claiming 16 bytes with nothing behind it.
static void wav_cut_short(const char *path)
{
    FILE *f = wav_start(path, 16, 1, 22050, 0);

    fclose(f);
    truncate(path, 20); // keep only RIFF/WAVE/fmt-id/size
}

// --- the loader, one WAV per process: the click is loaded once per audio thread --------

static void click_child(const char *wav, const char *dump)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) fail_at(__LINE__, "fork failed");
    if (pid == 0) {
        setenv("SHELF_AUDIO_CLICK", wav, 1);
        setenv("SHELF_AUDIO_DUMP", dump, 1);
        execlp(self, self, "click", (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid) fail_at(__LINE__, "waitpid failed");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "click child for %s failed (%d)", wav, status);
    CHECK(dump_bytes(dump) > 0, "no audio written for %s", wav);
}

static void test_click_wavs(void)
{
    char mono[600], stereo[600], eight[600], trunc[600], cut[600];
    char dmono[600], dstereo[600], deight[600], dtrunc[600], dcut[600];
    const double khz1[] = {1000, 1000}, khz2[] = {1000, 2500};
    const double half[] = {0.5, 0.5};
    size_t frames, audible;
    int16_t *s;

    snprintf(mono, sizeof mono, "%s/mono.wav", work);
    snprintf(stereo, sizeof stereo, "%s/stereo.wav", work);
    snprintf(eight, sizeof eight, "%s/8bit.wav", work);
    snprintf(trunc, sizeof trunc, "%s/trunc.wav", work);
    snprintf(cut, sizeof cut, "%s/cut.wav", work);
    snprintf(dmono, sizeof dmono, "%s/mono.raw", work);
    snprintf(dstereo, sizeof dstereo, "%s/stereo.raw", work);
    snprintf(deight, sizeof deight, "%s/8bit.raw", work);
    snprintf(dtrunc, sizeof dtrunc, "%s/trunc.raw", work);
    snprintf(dcut, sizeof dcut, "%s/cut.raw", work);

    // Mono 22.05 kHz, 1 kHz, 0.1 s: resampled to 48 kHz and doubled to both channels.
    wav_tone(mono, 1, 22050, khz1, half, 2205, 2205);
    click_child(mono, dmono);
    s = read_dump(dmono, &frames);
    audible = audible_frames(s, frames);
    CHECK(audible > 4080 && audible < 5520, "mono 22050 click lasts %zu frames, want ~4800", audible);
    CHECK(peak(s, audible) > 0.4, "mono 22050 click is too quiet (%.2f)", peak(s, audible));
    CHECK(tone_amp(s, audible, 0, 1000) > 0.35, "mono 22050 click lost its 1 kHz tone (%.2f)",
          tone_amp(s, audible, 0, 1000));
    CHECK(tone_amp(s, audible, 0, 2500) < 0.25 * tone_amp(s, audible, 0, 1000),
          "mono 22050 click is not 1 kHz");
    int diff = 0;

    for (size_t i = 0; i < audible; i++) {
        int d = abs(s[2 * i] - s[2 * i + 1]);

        if (d > diff) diff = d;
    }
    CHECK(diff <= 1, "the mono click differs between channels by %d", diff);
    free(s);

    // Stereo 44.1 kHz with a different tone per channel stays per channel.
    wav_tone(stereo, 2, 44100, khz2, half, 4410, 4410);
    click_child(stereo, dstereo);
    s = read_dump(dstereo, &frames);
    audible = audible_frames(s, frames);
    CHECK(audible > 4080 && audible < 5520, "stereo 44100 click lasts %zu frames, want ~4800", audible);
    CHECK(tone_amp(s, audible, 0, 1000) > 0.35, "stereo 44100 left channel lost its 1 kHz tone");
    CHECK(tone_amp(s, audible, 0, 2500) < 0.25 * tone_amp(s, audible, 0, 1000),
          "stereo 44100 left channel has the right channel's tone");
    CHECK(tone_amp(s, audible, 1, 2500) > 0.35, "stereo 44100 right channel lost its 2.5 kHz tone");
    CHECK(tone_amp(s, audible, 1, 1000) < 0.25 * tone_amp(s, audible, 1, 2500),
          "stereo 44100 right channel has the left channel's tone");
    free(s);

    // 8-bit is not 16-bit PCM: the backend falls back to its ~15 ms synthesized click.
    wav_8bit(eight, 22050, 2205);
    click_child(eight, deight);
    s = read_dump(deight, &frames);
    audible = audible_frames(s, frames);
    CHECK(audible > 500 && audible < 1000, "8-bit WAV was not rejected: %zu audible frames", audible);
    CHECK(peak(s, audible) > 0.5, "synthesized click is too quiet (%.2f)", peak(s, audible));
    free(s);

    // A data chunk that runs past the end of the file plays what is there.
    wav_tone(trunc, 1, 22050, khz1, half, 4410, 900);
    click_child(trunc, dtrunc);
    s = read_dump(dtrunc, &frames);
    audible = audible_frames(s, frames);
    CHECK(audible > 1400 && audible < 2600, "truncated WAV played %zu frames, want ~1959", audible);
    CHECK(peak(s, audible) > 0.1, "truncated WAV played silence");
    free(s);

    // A file cut inside its fmt chunk has no usable samples: synthesized click.
    wav_cut_short(cut);
    click_child(cut, dcut);
    s = read_dump(dcut, &frames);
    audible = audible_frames(s, frames);
    CHECK(audible > 500 && audible < 1000, "cut WAV was not rejected: %zu audible frames", audible);
    CHECK(peak(s, audible) > 0.5, "synthesized click after a cut WAV is too quiet");
    free(s);
}

// --- mixing, source swaps and idling --------------------------------------------------

static atomic_int sine_calls, tone_calls;
static double sine_phase, tone_phase;

static int16_t tone_sample(double *phase, double freq, double amp)
{
    int16_t v = (int16_t)lrint(amp * 32767.0 * sin(TWO_PI * freq * *phase / RATE));

    *phase += 1;
    return v;
}

static int sine_pull(int16_t *out, int frames)
{
    atomic_fetch_add(&sine_calls, 1);
    for (int i = 0; i < frames; i++) out[2 * i] = out[2 * i + 1] = tone_sample(&sine_phase, 440, 0.30);
    return frames;
}

static int tone_pull(int16_t *out, int frames)
{
    atomic_fetch_add(&tone_calls, 1);
    for (int i = 0; i < frames; i++) out[2 * i] = out[2 * i + 1] = tone_sample(&tone_phase, 660, 0.50);
    return frames;
}

static void test_mixing(void)
{
    char dump[600];
    size_t frames, off_b, off_c, off_d, off_e, still, later, off_f, off_g, sine_from, tone_to;
    int sine_at_swap, tone_at_null;
    int16_t *s;
    double t0, t_click, expected_bytes;

    snprintf(dump, sizeof dump, "%s/mix.raw", work);
    setenv("SHELF_AUDIO_DUMP", dump, 1);
    setenv("SHELF_AUDIO_CLICK", "/nonexistent/click.wav", 1); // synthesized click
    unlink(dump);

    t0 = now();
    platform_audio_source(sine_pull);
    sleep_until(t0 + 2.0);
    off_b = dump_bytes(dump);
    platform_audio_source(tone_pull); // swap: the 440 Hz source must stop here
    sine_at_swap = atomic_load(&sine_calls);
    sleep_for(0.4);
    off_c = dump_bytes(dump);
    tone_at_null = atomic_load(&tone_calls);
    platform_audio_source(NULL);
    sleep_for(0.2);
    CHECK(atomic_load(&sine_calls) == sine_at_swap, "the replaced source was called again");
    CHECK(atomic_load(&tone_calls) == tone_at_null, "the source was called after platform_audio_source(NULL)");
    CHECK(sine_at_swap > 0 && tone_at_null > 0, "a source was never pulled (%d, %d)", sine_at_swap, tone_at_null);

    // A click lands on top of the silence that follows.
    off_d = dump_bytes(dump);
    t_click = now();
    platform_click();
    sleep_for(0.3);
    off_e = dump_bytes(dump);
    CHECK(off_e > off_d, "nothing was written after the click");

    // Nothing is playing from here on: after ~2 s the thread blocks and stops writing.
    sleep_until(t_click + 2.6);
    still = dump_bytes(dump);
    sleep_for(1.0);
    later = dump_bytes(dump);
    CHECK(still == later, "the dump kept growing while idle (%zu -> %zu bytes)", still, later);

    // And a click brings the blocked thread back.
    off_f = later;
    platform_click();
    sleep_for(0.3);
    off_g = dump_bytes(dump);
    CHECK(off_g > off_f, "the thread did not wake up for a click after idling");

    s = read_dump(dump, &frames);
    expected_bytes = (t_click - t0 + 0.015 + 2.0) * RATE * 4; // click + 2 s of drain
    CHECK((double)still > expected_bytes * 0.97 && (double)still < expected_bytes * 1.03,
          "%zu bytes of audio for %.2f s of playback", still, expected_bytes / (RATE * 4));

    // 440 Hz at 0.3 of full scale in the first source's window.
    sine_from = 2400; // skip the thread's start-up
    CHECK(tone_amp(s + 2 * sine_from, 24000, 0, 440) > 0.25, "440 Hz source missing (%.2f)",
          tone_amp(s + 2 * sine_from, 24000, 0, 440));
    CHECK(tone_amp(s + 2 * sine_from, 24000, 0, 440) < 0.35, "440 Hz source is not 0.3 of full scale");
    CHECK(tone_amp(s + 2 * sine_from, 24000, 1, 440) > 0.25, "440 Hz source missing on the right channel");
    CHECK(tone_amp(s + 2 * sine_from, 24000, 0, 1000) < 0.05, "the 440 Hz window is not clean");

    // After the swap the 660 Hz source, at 0.5 of full scale.
    tone_to = off_c / 4 - 960; // the swap is heard in the next chunk
    CHECK(tone_to > off_b / 4 + 960, "the second source's window is empty");
    CHECK(tone_amp(s + 2 * (off_b / 4 + 960), tone_to - (off_b / 4 + 960), 0, 660) > 0.4,
          "660 Hz source missing (%.2f)",
          tone_amp(s + 2 * (off_b / 4 + 960), tone_to - (off_b / 4 + 960), 0, 660));
    CHECK(tone_amp(s + 2 * (off_b / 4 + 960), tone_to - (off_b / 4 + 960), 0, 440) < 0.1,
          "the replaced 440 Hz source is still audible");

    // The click's 0.9 peak on top of nothing: the mixed output must reach it.
    CHECK(peak(s + 2 * (off_d / 4), off_e / 4 - off_d / 4) > 0.8, "the click did not reach the output (%.2f)",
          peak(s + 2 * (off_d / 4), off_e / 4 - off_d / 4));
    CHECK(peak(s + 2 * (off_f / 4), off_g / 4 - off_f / 4) > 0.8, "the click after idling is missing (%.2f)",
          peak(s + 2 * (off_f / 4), off_g / 4 - off_f / 4));
    free(s);
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");

    self = argv[0];
    if (argc > 1 && strcmp(argv[1], "click") == 0) { // one loader case, fresh process
        platform_click();
        sleep_for(0.4);
        return 0;
    }
    if (argc > 1) {
        snprintf(work, sizeof work, "%s", argv[1]);
    } else {
        if (!tmp || !*tmp) tmp = "/tmp";
        snprintf(work, sizeof work, "%s/shelf-audio-%ld", tmp, (long)getpid());
    }
    if (mkdir(work, 0700) && errno != EEXIST) fail_at(__LINE__, "cannot create %s", work);

    test_click_wavs();
    test_mixing();
    remove_tree(work);

    printf("all %d checks passed\n", check_count);
    return 0;
}
