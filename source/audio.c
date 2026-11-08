#include <3ds.h>
#include <string.h>

#include "audio.h"
#include "dr_mp3.h"

#define CHANNEL     0
#define NUM_BUFS    3
#define BUF_FRAMES  4096

static ndspWaveBuf wavebufs[NUM_BUFS];
static s16 *pcm;

static drmp3 mp3;
static bool loaded, eof, paused;
static u32 channels, sample_rate;
static u64 frames_queued;
static u64 total_frames;
static bool length_exact;

static Thread thread;
static LightEvent event;
static LightLock lock;
static volatile bool running;

static void ndsp_callback(void *arg)
{
    (void)arg;
    LightEvent_Signal(&event);
}

static void estimate_length(void)
{
    if (mp3.streamLength == DRMP3_UINT64_MAX || mp3.streamCursor < mp3.dataSize)
        return;
    u64 cursor = mp3.streamCursor - mp3.dataSize;
    if (cursor <= mp3.streamStartOffset || mp3.streamLength <= mp3.streamStartOffset)
        return;
    u64 consumed = cursor - mp3.streamStartOffset;
    total_frames = mp3.currentPCMFrame * (mp3.streamLength - mp3.streamStartOffset) / consumed;
}

static void fill_buffers(void)
{
    if (!loaded || eof)
        return;

    for (int i = 0; i < NUM_BUFS; i++) {
        ndspWaveBuf *wb = &wavebufs[i];
        if (wb->status != NDSP_WBUF_FREE && wb->status != NDSP_WBUF_DONE)
            continue;

        drmp3_uint64 n = drmp3_read_pcm_frames_s16(&mp3, BUF_FRAMES, wb->data_pcm16);
        if (n == 0) {
            eof = true;
            if (!length_exact) {
                total_frames = frames_queued;
                length_exact = true;
            }
            break;
        }

        wb->nsamples = n;
        DSP_FlushDataCache(wb->data_pcm16, n * channels * sizeof(s16));
        ndspChnWaveBufAdd(CHANNEL, wb);
        frames_queued += n;
    }

    if (!length_exact)
        estimate_length();
}

static void audio_thread(void *arg)
{
    (void)arg;
    while (running) {
        LightLock_Lock(&lock);
        fill_buffers();
        LightLock_Unlock(&lock);
        LightEvent_Wait(&event);
    }
}

static void clear_buffers(void)
{
    ndspChnWaveBufClear(CHANNEL);
    for (int i = 0; i < NUM_BUFS; i++)
        wavebufs[i].status = NDSP_WBUF_DONE;
}

static void stop_locked(void)
{
    clear_buffers();

    if (loaded)
        drmp3_uninit(&mp3);
    loaded = false;
    eof = false;
    frames_queued = 0;
    total_frames = 0;
    length_exact = false;
}

bool audio_init(void)
{
    if (R_FAILED(ndspInit()))
        return false;

    pcm = linearAlloc(NUM_BUFS * BUF_FRAMES * 2 * sizeof(s16));
    if (!pcm) {
        ndspExit();
        return false;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    memset(wavebufs, 0, sizeof(wavebufs));
    for (int i = 0; i < NUM_BUFS; i++)
        wavebufs[i].data_pcm16 = pcm + i * BUF_FRAMES * 2;

    LightEvent_Init(&event, RESET_ONESHOT);
    LightLock_Init(&lock);
    ndspSetCallback(ndsp_callback, NULL);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    running = true;
    thread = threadCreate(audio_thread, NULL, 64 * 1024, prio - 1, -1, false);
    if (!thread) {
        running = false;
        linearFree(pcm);
        ndspExit();
        return false;
    }
    return true;
}

void audio_exit(void)
{
    running = false;
    LightEvent_Signal(&event);
    threadJoin(thread, U64_MAX);
    threadFree(thread);

    LightLock_Lock(&lock);
    stop_locked();
    LightLock_Unlock(&lock);

    ndspSetCallback(NULL, NULL);
    linearFree(pcm);
    ndspExit();
}

bool audio_play(const char *path)
{
    LightLock_Lock(&lock);
    stop_locked();

    if (!drmp3_init_file(&mp3, path, NULL) || mp3.channels < 1 || mp3.channels > 2) {
        LightLock_Unlock(&lock);
        return false;
    }
    loaded = true;
    channels = mp3.channels;
    sample_rate = mp3.sampleRate;
    if (mp3.totalPCMFrameCount != DRMP3_UINT64_MAX) {
        total_frames = mp3.totalPCMFrameCount;
        length_exact = true;
    }

    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;

    ndspChnSetInterp(CHANNEL, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(CHANNEL, sample_rate);
    ndspChnSetFormat(CHANNEL, channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetMix(CHANNEL, mix);

    paused = false;
    ndspChnSetPaused(CHANNEL, false);

    fill_buffers();
    LightLock_Unlock(&lock);
    return true;
}

void audio_toggle_pause(void)
{
    paused = !paused;
    ndspChnSetPaused(CHANNEL, paused);
}

static bool seek_to_frame(u64 frame)
{
    u64 start = mp3.streamStartOffset, end = mp3.streamLength;
    if (frame == 0 || total_frames == 0 || end == DRMP3_UINT64_MAX || end <= start)
        return drmp3_seek_to_pcm_frame(&mp3, frame);

    static drmp3_seek_point point;
    point = (drmp3_seek_point){
        .seekPosInBytes = start + (end - start) * frame / total_frames,
        .pcmFrameIndex = frame,
        .mp3FramesToDiscard = 1,
    };
    drmp3_bind_seek_table(&mp3, 1, &point);
    bool ok = drmp3_seek_to_pcm_frame(&mp3, frame);
    drmp3_bind_seek_table(&mp3, 0, NULL);
    return ok;
}

void audio_seek(float seconds)
{
    LightLock_Lock(&lock);
    if (!loaded) {
        LightLock_Unlock(&lock);
        return;
    }

    u64 frame = seconds > 0.0f ? (u64)(seconds * sample_rate) : 0;
    if (total_frames && frame >= total_frames)
        frame = total_frames - 1;

    clear_buffers();

    if (!seek_to_frame(frame))
        frame = mp3.currentPCMFrame;
    frames_queued = frame;
    eof = false;

    ndspChnSetPaused(CHANNEL, paused);
    fill_buffers();
    LightLock_Unlock(&lock);
}

bool audio_is_paused(void)
{
    return paused;
}

bool audio_is_finished(void)
{
    LightLock_Lock(&lock);
    bool finished = loaded && eof;
    for (int i = 0; finished && i < NUM_BUFS; i++) {
        u8 s = wavebufs[i].status;
        if (s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING)
            finished = false;
    }
    LightLock_Unlock(&lock);
    return finished;
}

float audio_position(void)
{
    LightLock_Lock(&lock);
    u64 pending = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        u8 s = wavebufs[i].status;
        if (s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING)
            pending += wavebufs[i].nsamples;
    }
    float pos = (loaded && sample_rate) ? (float)(frames_queued - pending) / sample_rate : 0.0f;
    LightLock_Unlock(&lock);
    return pos;
}

float audio_duration(void)
{
    LightLock_Lock(&lock);
    float len = (loaded && sample_rate) ? (float)total_frames / sample_rate : 0.0f;
    LightLock_Unlock(&lock);
    return len;
}

u32 audio_sample_rate(void)
{
    return sample_rate;
}

int audio_get_samples(float *out, int n)
{
    if (paused)
        return 0;

    LightLock_Lock(&lock);
    ndspWaveBuf *wb = NULL;
    for (int i = 0; i < NUM_BUFS; i++)
        if (wavebufs[i].status == NDSP_WBUF_PLAYING)
            wb = &wavebufs[i];

    int got = 0;
    if (loaded && wb && wb->nsamples > 0) {
        int avail = wb->nsamples;
        if (n > avail)
            n = avail;
        int start = ndspChnGetSamplePos(CHANNEL);
        if (start > avail - n)
            start = avail - n;
        if (start < 0)
            start = 0;

        const s16 *src = wb->data_pcm16 + start * channels;
        if (channels == 2) {
            for (int i = 0; i < n; i++)
                out[i] = (src[2 * i] + src[2 * i + 1]) * (0.5f / 32768.0f);
        } else {
            for (int i = 0; i < n; i++)
                out[i] = src[i] * (1.0f / 32768.0f);
        }
        got = n;
    }
    LightLock_Unlock(&lock);
    return got;
}
