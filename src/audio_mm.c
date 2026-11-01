// Miyoo audio: one lazily started thread mixes the game's source (platform_audio_source)
// with the UI click and sends interleaved stereo s16 at 48 kHz to MI_AO device 0,
// channel 0. libmi_sys/libmi_ao are dlopen'ed, so the same binary also runs in the VM and
// on the Mac host, where the mix goes to SHELF_AUDIO_DUMP (a raw s16le file) or, with
// neither, nowhere — still pulled and paced by the clock so the game's own rate control
// sees a real-time consumer.
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform.h"

#define TWO_PI 6.283185307179586477

// ---------------------------------------------------------------------------------------
// SigmaStar SDK types, copied verbatim from the device toolchain's headers
// (/opt/miyoomini-toolchain/arm-linux-gnueabihf/libc/usr/include): MI_S32, MI_U32, MI_U64
// and MI_BOOL from mi_common_datatype.h, the rest from mi_aio_datatype.h, which
// mi_ao.h includes. The layouts must match libmi_ao.so's ABI byte for byte.
// ---------------------------------------------------------------------------------------
typedef unsigned char MI_BOOL;
typedef unsigned int MI_U32;
typedef unsigned long long MI_U64;
typedef int MI_S32;

#define MI_SUCCESS (0)
#define MI_AUDIO_MAX_CHN_NUM (16)

typedef enum
{
    E_MI_AUDIO_SAMPLE_RATE_8000 = 8000,         /* 8kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_11025 = 11025,       /* 11.025kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_12000 = 12000,       /* 12kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_16000 = 16000,       /* 16kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_22050 = 22050,       /* 22.05kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_24000 = 24000,       /* 24kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_32000 = 32000,       /* 32kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_44100 = 44100,       /* 44.1kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_48000 = 48000,       /* 48kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_96000 = 96000,       /* 96kHz sampling rate */
    E_MI_AUDIO_SAMPLE_RATE_INVALID,
} MI_AUDIO_SampleRate_e;

typedef enum
{
    E_MI_AUDIO_BIT_WIDTH_16 = 0,                /* 16bit width */
    E_MI_AUDIO_BIT_WIDTH_24 = 1,                /* 24bit width */
    E_MI_AUDIO_BIT_WIDTH_MAX,
} MI_AUDIO_BitWidth_e;

typedef enum
{
    E_MI_AUDIO_MODE_I2S_MASTER,                 /* I2S master mode */
    E_MI_AUDIO_MODE_I2S_SLAVE,                  /* I2S slave mode */
    E_MI_AUDIO_MODE_TDM_MASTER,                 /* TDM master mode */
    E_MI_AUDIO_MODE_TDM_SLAVE,                  /* TDM slave mode */
    E_MI_AUDIO_MODE_MAX,
} MI_AUDIO_Mode_e;

typedef enum
{
    E_MI_AUDIO_SOUND_MODE_MONO = 0,             /* mono */
    E_MI_AUDIO_SOUND_MODE_STEREO = 1,           /* stereo */
    E_MI_AUDIO_SOUND_MODE_QUEUE = 2,            /* all data in One chn */
    E_MI_AUDIO_SOUND_MODE_MAX,
} MI_AUDIO_SoundMode_e;

typedef enum
{
    E_MI_AUDIO_I2S_FMT_I2S_MSB,
    E_MI_AUDIO_I2S_FMT_LEFT_JUSTIFY_MSB,
} MI_AUDIO_I2sFmt_e;

typedef enum
{
    E_MI_AUDIO_I2S_MCLK_0,                      /* not enable */
    E_MI_AUDIO_I2S_MCLK_12_288M,                /* 12.288M */
    E_MI_AUDIO_I2S_MCLK_16_384M,                /* 16.384M */
    E_MI_AUDIO_I2S_MCLK_18_432M,                /* 18.432M */
    E_MI_AUDIO_I2S_MCLK_24_576M,                /* 24.576M */
    E_MI_AUDIO_I2S_MCLK_24M,                    /* 24MHz */
    E_MI_AUDIO_I2S_MCLK_48M,                    /* 48MHz */
} MI_AUDIO_I2sMclk_e;

typedef struct MI_AUDIO_I2sConfig_s
{
    MI_AUDIO_I2sFmt_e eFmt;
    MI_AUDIO_I2sMclk_e eMclk;
    MI_BOOL bSyncClock;                         /* I2s Tx BCLK, Rx BCLK use the same clock source */
} MI_AUDIO_I2sConfig_t;

typedef struct MI_AUDIO_Attr_s
{
    MI_AUDIO_SampleRate_e eSamplerate;          /* sample rate */
    MI_AUDIO_BitWidth_e eBitwidth;              /* bitwidth */
    MI_AUDIO_Mode_e eWorkmode;                  /* I2s master or slave mode */
    MI_AUDIO_SoundMode_e eSoundmode;            /* mono or stereo */
    MI_U32 u32FrmNum;                           /* frame num in buffer */
    MI_U32 u32PtNumPerFrm;                      /* number of samples */
    MI_U32 u32CodecChnCnt;                      /* channel number on Codec */
    MI_U32 u32ChnCnt;
    union {
        MI_AUDIO_I2sConfig_t stI2sConfig;
    } WorkModeSetting;
} MI_AUDIO_Attr_t;

typedef struct MI_AUDIO_Frame_s
{
    MI_AUDIO_BitWidth_e eBitwidth;              /* audio frame bitwidth */
    MI_AUDIO_SoundMode_e eSoundmode;            /* audio frame momo or stereo mode */
    void *apVirAddr[MI_AUDIO_MAX_CHN_NUM];
    MI_U64 u64TimeStamp;                        /* audio frame timestamp */
    MI_U32 u32Seq;                              /* audio frame seq */
    MI_U32 u32Len;                              /* data lenth per channel in frame */
    MI_U32 au32PoolId[2];                       /* Only use for Ai */
    void *apSrcPcmVirAddr[MI_AUDIO_MAX_CHN_NUM]; /* Ai original pcm data from ai channel */
    MI_U32 u32SrcPcmLen;                        /* length of Ai original pcm data */
} MI_AUDIO_Frame_t;

// ---------------------------------------------------------------------------------------
// MI_AO backend
// ---------------------------------------------------------------------------------------
#define MI_AO_DEV 0
#define MI_AO_CHN 0
#define MI_FRAMES 1024        // samples per SendFrame (~21 ms at 48 kHz)
#define MI_SEND_TIMEOUT_MS 30 // SendFrame blocks until the AO buffer has room: that is the clock
#define MI_ERRORS_MAX 10      // consecutive failed sends before giving up on the hardware

static struct {
    void *sys_lib, *ao_lib;
    MI_S32 (*sys_init)(void);
    MI_S32 (*set_pub_attr)(MI_S32 dev, MI_AUDIO_Attr_t *attr);
    MI_S32 (*enable)(MI_S32 dev);
    MI_S32 (*disable)(MI_S32 dev);
    MI_S32 (*enable_chn)(MI_S32 dev, MI_S32 chn);
    MI_S32 (*disable_chn)(MI_S32 dev, MI_S32 chn);
    MI_S32 (*send_frame)(MI_S32 dev, MI_S32 chn, MI_AUDIO_Frame_t *frame, MI_S32 timeout_ms);
    MI_S32 (*set_volume)(MI_S32 dev, MI_S32 db);
    MI_U32 seq;
} mi;

// The plain names honour LD_LIBRARY_PATH (`/lib:/config/lib:/customer/lib`, set by the
// updater); the two explicit paths cover a binary started without it.
static void *lib_open(const char *name)
{
    static const char *const dirs[] = {"", "/config/lib/", "/customer/lib/"};
    char path[64];

    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s%s", dirs[i], name);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
}

// Returns 0 when both libraries and every entry point we use are there.
static int mi_load(void)
{
    mi.sys_lib = lib_open("libmi_sys.so");
    mi.ao_lib = lib_open("libmi_ao.so");
    if (mi.sys_lib && mi.ao_lib) {
        mi.sys_init = (MI_S32 (*)(void))dlsym(mi.sys_lib, "MI_SYS_Init");
        mi.set_pub_attr = (MI_S32 (*)(MI_S32, MI_AUDIO_Attr_t *))dlsym(mi.ao_lib, "MI_AO_SetPubAttr");
        mi.enable = (MI_S32 (*)(MI_S32))dlsym(mi.ao_lib, "MI_AO_Enable");
        mi.disable = (MI_S32 (*)(MI_S32))dlsym(mi.ao_lib, "MI_AO_Disable");
        mi.enable_chn = (MI_S32 (*)(MI_S32, MI_S32))dlsym(mi.ao_lib, "MI_AO_EnableChn");
        mi.disable_chn = (MI_S32 (*)(MI_S32, MI_S32))dlsym(mi.ao_lib, "MI_AO_DisableChn");
        mi.send_frame = (MI_S32 (*)(MI_S32, MI_S32, MI_AUDIO_Frame_t *, MI_S32))dlsym(
            mi.ao_lib, "MI_AO_SendFrame");
        mi.set_volume = (MI_S32 (*)(MI_S32, MI_S32))dlsym(mi.ao_lib, "MI_AO_SetVolume");
        if (mi.sys_init && mi.set_pub_attr && mi.enable && mi.disable && mi.enable_chn &&
            mi.disable_chn && mi.send_frame && mi.set_volume)
            return 0;
    }
    if (mi.sys_lib) dlclose(mi.sys_lib);
    if (mi.ao_lib) dlclose(mi.ao_lib);
    memset(&mi, 0, sizeof mi);
    return -1;
}

// The 0-20 level of docs/hardware.md as dB, so audio starts at the user's volume instead of
// the SDK's default. Level 0 muting is the volume path's job (hw_mm.c).
static void mi_apply_volume(void)
{
    int level = platform_volume();
    int db;

    if (level < 0) level = 0;
    if (level > 20) level = 20;
    db = (int)lround(48.0 * log10(1.0 + level)) - 60;
    mi.set_volume(MI_AO_DEV, db);
}

// Opens the AO device and channel. Returns 0 when frames can be sent.
static int mi_start(void)
{
    MI_AUDIO_Attr_t attr;

    if (mi_load() != 0 || mi.sys_init() != MI_SUCCESS) return -1;
    memset(&attr, 0, sizeof attr);
    attr.eSamplerate = E_MI_AUDIO_SAMPLE_RATE_48000;
    attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
    attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
    attr.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
    // Unverified on hardware: u32FrmNum (buffer depth), u32CodecChnCnt (codec outputs) and
    // u32ChnCnt (1 = one AO channel carrying an interleaved stereo stream), the I2S format
    // (MSB, no MCLK: the codec clocks itself), the SendFrame timeout (longer than one
    // frame, so a send only fails when the device is really stuck) and the volume dB
    // mapping. They are config-time constants, so a wrong one shows up as silence or as an
    // error from MI_AO_SetPubAttr, not as corrupted audio.
    attr.u32FrmNum = 4;
    attr.u32PtNumPerFrm = MI_FRAMES;
    attr.u32CodecChnCnt = 2;
    attr.u32ChnCnt = 1;
    attr.WorkModeSetting.stI2sConfig.eFmt = E_MI_AUDIO_I2S_FMT_I2S_MSB;
    attr.WorkModeSetting.stI2sConfig.eMclk = E_MI_AUDIO_I2S_MCLK_0;
    attr.WorkModeSetting.stI2sConfig.bSyncClock = 0;
    if (mi.set_pub_attr(MI_AO_DEV, &attr) != MI_SUCCESS || mi.enable(MI_AO_DEV) != MI_SUCCESS ||
        mi.enable_chn(MI_AO_DEV, MI_AO_CHN) != MI_SUCCESS) {
        mi.disable_chn(MI_AO_DEV, MI_AO_CHN); // both are no-ops if the step that failed was first
        mi.disable(MI_AO_DEV);
        return -1;
    }
    mi_apply_volume();
    return 0;
}

// Sends one frame. Returns 0 when the hardware took it (and so paced us), -1 otherwise.
static int mi_send(const int16_t *samples, int frames)
{
    MI_AUDIO_Frame_t frame;

    memset(&frame, 0, sizeof frame);
    frame.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
    frame.eSoundmode = E_MI_AUDIO_SOUND_MODE_STEREO;
    frame.apVirAddr[0] = (void *)samples;
    frame.u32Len = (MI_U32)frames * (MI_U32)sizeof(int16_t); // bytes per channel
    frame.u32Seq = mi.seq++;
    return mi.send_frame(MI_AO_DEV, MI_AO_CHN, &frame, MI_SEND_TIMEOUT_MS) == MI_SUCCESS ? 0 : -1;
}

// ---------------------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------------------
static double mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void sleep_until(double t)
{
    double d = t - mono_now();

    while (d > 0) {
        struct timespec ts;

        ts.tv_sec = (time_t)d;
        ts.tv_nsec = (long)((d - (double)(time_t)d) * 1e9);
        if (ts.tv_nsec > 999999999) ts.tv_nsec = 999999999;
        nanosleep(&ts, NULL);
        d = t - mono_now();
    }
}

// ---------------------------------------------------------------------------------------
// Mixing state: the game source, the click and the idle handshake share one mutex. The
// source is pulled with it held, so platform_audio_source() can swap it out and be sure
// the old one is never called again once it returns.
// ---------------------------------------------------------------------------------------
#define CLICK_WAV "/mnt/SDCARD/.shelf/sound/click.wav"
#define CLICK_SYNTH_FRAMES 720 // 15 ms at 48 kHz
#define IDLE_AFTER 2.0         // seconds of nothing to play before the thread blocks
#define SINK_FRAMES 480        // 10 ms per dump/null write
#define PACE_LAG_MAX 0.25      // catch up at most this much after a stall, then re-sync

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int (*source)(int16_t *out, int frames);
static int16_t *click;
static int click_frames, click_pos, click_active;
static int click_loaded;  // the audio thread has tried the WAV (and fallen back if needed)
static int click_pending; // a click arrived before that

static void sat_add(int16_t *dst, int16_t add)
{
    int v = *dst + add;

    *dst = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
}

// Fills `out` with the source plus the click. Called and returns with `lock` held.
static void mix(int16_t *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    if (source) source(out, frames);
    for (int i = 0; i < frames && click_active; i++) {
        for (int c = 0; c < 2; c++) sat_add(&out[2 * i + c], click[2 * click_pos + c]);
        if (++click_pos >= click_frames) click_active = 0;
    }
}

// ---------------------------------------------------------------------------------------
// Click WAV
// ---------------------------------------------------------------------------------------
static unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
           (unsigned long)p[3] << 24;
}

// Converts any 16-bit PCM WAV (mono or stereo, any rate) to 48 kHz stereo with linear
// interpolation. Returns NULL for anything else: 8-bit, compressed, no fmt or data chunk,
// or a file cut short before its samples. The voice-channel details a WAV can carry
// (WAVE_FORMAT_EXTENSIBLE) are not expected from a click file and are rejected.
static int16_t *wav_load(const char *path, int *out_frames)
{
    enum { WAV_MAX_BYTES = 4 << 20 };
    unsigned char *buf = NULL;
    int16_t *out = NULL;
    const unsigned char *data = NULL;
    size_t size, data_len = 0, in_frames, frames, i;
    unsigned channels = 0, bits = 0, format = 0;
    unsigned long rate = 0;
    double step;
    FILE *f = fopen(path, "rb");
    long len;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) goto done;
    size = (size_t)len > WAV_MAX_BYTES ? WAV_MAX_BYTES : (size_t)len;
    buf = malloc(size + 1);
    if (!buf) goto done;
    size = fread(buf, 1, size, f);
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) goto done;

    // Walk the chunks. A size that runs past the end of the file (a truncated download)
    // is clamped to what is there instead of rejected, so the click still plays.
    for (size_t off = 12; off + 8 <= size;) {
        const unsigned char *id = buf + off;
        size_t chunk = le32(buf + off + 4);

        off += 8;
        if (chunk > size - off) chunk = size - off;
        if (!memcmp(id, "fmt ", 4)) {
            if (chunk >= 16) {
                format = le16(buf + off);
                channels = le16(buf + off + 2);
                rate = le32(buf + off + 4);
                bits = le16(buf + off + 14);
            }
        } else if (!memcmp(id, "data", 4)) {
            data = buf + off;
            data_len = chunk;
        }
        off += chunk + (chunk & 1); // RIFF pads odd chunks to an even length
    }
    if (!data || format != 1 || bits != 16 || (channels != 1 && channels != 2) ||
        rate < 1 || rate > 192000)
        goto done;
    in_frames = data_len / (2 * channels);
    if (in_frames == 0) goto done;

    frames = (size_t)((double)in_frames * PLATFORM_AUDIO_RATE / (double)rate + 0.5);
    if (frames == 0) frames = 1;
    out = malloc(frames * 2 * sizeof *out);
    if (!out) goto done;
    step = (double)rate / PLATFORM_AUDIO_RATE;
    for (i = 0; i < frames; i++) {
        size_t i0 = (size_t)((double)i * step), i1;
        float frac;

        if (i0 >= in_frames) i0 = in_frames - 1;
        i1 = i0 + 1 < in_frames ? i0 + 1 : i0;
        frac = (float)((double)i * step - (double)i0);
        for (int c = 0; c < 2; c++) {
            size_t sc = (size_t)(c < (int)channels ? c : 0);
            float v0 = (float)(int16_t)le16(data + (i0 * channels + sc) * 2);
            float v1 = (float)(int16_t)le16(data + (i1 * channels + sc) * 2);

            out[2 * i + c] = (int16_t)lrintf(v0 + (v1 - v0) * frac);
        }
    }
    *out_frames = (int)frames;
done:
    free(buf);
    fclose(f);
    return out; // NULL when buf was freed above and out was never allocated
}

// Fallback when there is no usable click WAV: a short decaying tick.
static int16_t *click_synth(int *out_frames)
{
    int16_t *out = malloc(CLICK_SYNTH_FRAMES * 2 * sizeof *out);

    if (!out) return NULL;
    for (int i = 0; i < CLICK_SYNTH_FRAMES; i++) {
        double env = exp(-(double)i / (0.003 * PLATFORM_AUDIO_RATE)); // 3 ms decay
        int16_t v = (int16_t)lrint(0.9 * 32767.0 * env * sin(TWO_PI * 1200.0 * i / PLATFORM_AUDIO_RATE));

        out[2 * i] = out[2 * i + 1] = v;
    }
    *out_frames = CLICK_SYNTH_FRAMES;
    return out;
}

// Loaded once, on the audio thread, before the first frame. SHELF_AUDIO_CLICK points it at
// another file so the host test can keep its generated WAVs out of /mnt/SDCARD.
static void click_load(void)
{
    const char *path = getenv("SHELF_AUDIO_CLICK");

    if (!path || !*path) path = CLICK_WAV;
    click = wav_load(path, &click_frames);
    if (!click) click = click_synth(&click_frames);
    if (!click) click_frames = 0;
}

// ---------------------------------------------------------------------------------------
// Sinks: MI_AO, SHELF_AUDIO_DUMP, or none. Chosen once, on the audio thread.
// ---------------------------------------------------------------------------------------
enum { SINK_NONE, SINK_MI, SINK_DUMP };

static int sink;
static FILE *dump;
static int mi_errors;
static double next_write;
static int sink_frames;

static int sink_open(void)
{
    const char *path = getenv("SHELF_AUDIO_DUMP");

    if (mi_start() == 0) {
        sink_frames = MI_FRAMES;
        return sink = SINK_MI;
    }
    sink_frames = SINK_FRAMES;
    if (path && *path && (dump = fopen(path, "wb"))) {
        setvbuf(dump, NULL, _IONBF, 0); // the test watches the size grow
        return sink = SINK_DUMP;
    }
    return sink = SINK_NONE;
}

// Writes one frame. MI_AO's SendFrame blocks until the buffer has room, so it is the clock
// there; the file and null sinks sleep to the monotonic clock instead.
static void sink_write(const int16_t *samples, int frames)
{
    double now;

    if (sink == SINK_MI) {
        if (mi_send(samples, frames) == 0) {
            mi_errors = 0;
            return;
        }
        if (++mi_errors < MI_ERRORS_MAX) return;
        // The device stopped taking frames (standby, a crashed audioserver): stop sending
        // rather than spin, and carry on clock-paced and silent.
        mi.disable_chn(MI_AO_DEV, MI_AO_CHN);
        mi.disable(MI_AO_DEV);
        sink = SINK_NONE;
    } else if (sink == SINK_DUMP && dump) {
        fwrite(samples, 2 * sizeof *samples, (size_t)frames, dump);
    }
    now = mono_now();
    if (next_write < now - PACE_LAG_MAX) next_write = now; // stalled: re-sync instead of bursting
    next_write += (double)frames / PLATFORM_AUDIO_RATE;
    sleep_until(next_write);
}

// Idle: MI_AO's channel is closed and reopened with it, so a silent Shelf holds no AO
// buffer and no DMA.
static void sink_idle(void)
{
    if (sink == SINK_MI) mi.disable_chn(MI_AO_DEV, MI_AO_CHN);
}

static void sink_resume(void)
{
    if (sink == SINK_MI) mi.enable_chn(MI_AO_DEV, MI_AO_CHN);
    next_write = mono_now();
}

// ---------------------------------------------------------------------------------------
// The audio thread
// ---------------------------------------------------------------------------------------
static void *audio_thread(void *unused)
{
    int16_t *buf;
    double last_active;

    (void)unused;
    click_load();
    pthread_mutex_lock(&lock);
    if (click_pending) {
        click_pos = 0;
        click_active = click_frames > 0;
    }
    click_loaded = 1;
    click_pending = 0;
    pthread_mutex_unlock(&lock);
    sink_open();
    buf = malloc((size_t)(sink_frames > MI_FRAMES ? sink_frames : MI_FRAMES) * 2 * sizeof *buf);
    if (!buf) return NULL;
    next_write = mono_now();
    last_active = next_write;

    for (;;) {
        int frames;

        pthread_mutex_lock(&lock);
        if (!source && !click_active && mono_now() - last_active >= IDLE_AFTER) {
            sink_idle();
            while (!source && !click_active) pthread_cond_wait(&wake, &lock);
            sink_resume();
            last_active = mono_now();
        }
        frames = sink_frames;
        mix(buf, frames);
        if (source || click_active) last_active = mono_now();
        pthread_mutex_unlock(&lock);
        sink_write(buf, frames);
    }
    return NULL; // not reached
}

static void thread_start(void)
{
    pthread_attr_t attr;
    pthread_t t;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, audio_thread, NULL);
    pthread_attr_destroy(&attr);
}

static pthread_once_t once = PTHREAD_ONCE_INIT;

void platform_audio_source(int (*pull)(int16_t *out, int frames))
{
    pthread_once(&once, thread_start);
    pthread_mutex_lock(&lock);
    source = pull;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void platform_click(void)
{
    pthread_once(&once, thread_start);
    pthread_mutex_lock(&lock);
    click_pos = 0;
    if (click_loaded) {
        click_active = click_frames > 0;
    } else {
        click_pending = 1; // the thread is still loading the WAV; it starts the click
    }
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}
