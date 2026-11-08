#include <3ds.h>
#include <math.h>

#include "audio.h"
#include "visualizer.h"

#define FFT_BITS    10
#define FFT_N       (1 << FFT_BITS)

#define FREQ_LO     50.0f
#define FREQ_HI     16000.0f
#define DB_FLOOR    -54.0f
#define DB_RANGE    48.0f
#define TILT_DB     3.0f

#define RISE        0.7f
#define FALL        0.035f
#define PEAK_HOLD   24
#define PEAK_FALL   0.015f

typedef struct {
    float level, peak;
    int hold;
} Meter;

static float window[FFT_N];
static float cos_tab[FFT_N / 2], sin_tab[FFT_N / 2];
static float re[FFT_N], im[FFT_N];
static float samples[FFT_N];
static bool ready;

static u32 band_rate;
static int band_lo[VIZ_BARS], band_hi[VIZ_BARS];
static float band_tilt[VIZ_BARS];

static Meter bars[VIZ_BARS];

static void init_tables(void)
{
    for (int i = 0; i < FFT_N; i++)
        window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
    for (int i = 0; i < FFT_N / 2; i++) {
        cos_tab[i] = cosf(2.0f * (float)M_PI * i / FFT_N);
        sin_tab[i] = -sinf(2.0f * (float)M_PI * i / FFT_N);
    }
    ready = true;
}

static void init_bands(u32 rate)
{
    float hi = FREQ_HI < rate * 0.5f ? FREQ_HI : rate * 0.5f;
    int prev = 1;
    for (int b = 0; b < VIZ_BARS; b++) {
        float f0 = FREQ_LO * powf(hi / FREQ_LO, (float)b / VIZ_BARS);
        float f1 = FREQ_LO * powf(hi / FREQ_LO, (float)(b + 1) / VIZ_BARS);
        int lo = (int)(f0 * FFT_N / rate);
        int up = (int)(f1 * FFT_N / rate);
        if (lo < prev)
            lo = prev;
        if (up <= lo)
            up = lo + 1;
        if (up > FFT_N / 2)
            up = FFT_N / 2;
        band_lo[b] = lo;
        band_hi[b] = up;
        prev = up;
        band_tilt[b] = TILT_DB * log2f(sqrtf(f0 * f1) / 1000.0f);
    }
    band_rate = rate;
}

static void fft(void)
{
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1, step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = cos_tab[k * step], wi = sin_tab[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void analyze_spectrum(u32 rate, float *target)
{
    if (rate != band_rate)
        init_bands(rate);

    for (int i = 0; i < FFT_N; i++) {
        re[i] = samples[i] * window[i];
        im[i] = 0.0f;
    }
    fft();

    const float norm = (4.0f / FFT_N) * (4.0f / FFT_N);
    for (int b = 0; b < VIZ_BARS; b++) {
        float power = 0.0f;
        for (int k = band_lo[b]; k < band_hi[b]; k++) {
            float p = re[k] * re[k] + im[k] * im[k];
            if (p > power)
                power = p;
        }
        float db = 10.0f * log10f(power * norm + 1e-12f) + band_tilt[b];
        target[b] = clamp01((db - DB_FLOOR) / DB_RANGE);
    }
}

static void meter_step(Meter *m, float target)
{
    if (target > m->level) {
        m->level += (target - m->level) * RISE;
    } else {
        m->level -= FALL;
        if (m->level < target)
            m->level = target;
    }

    if (m->level >= m->peak) {
        m->peak = m->level;
        m->hold = PEAK_HOLD;
    } else if (m->hold > 0) {
        m->hold--;
    } else {
        m->peak -= PEAK_FALL;
        if (m->peak < m->level)
            m->peak = m->level;
    }
}

void viz_update(void)
{
    float target[VIZ_BARS] = {0};

    int n = audio_get_samples(samples, FFT_N);
    u32 rate = audio_sample_rate();
    if (n == FFT_N && rate > 0) {
        if (!ready)
            init_tables();
        analyze_spectrum(rate, target);
    }

    for (int b = 0; b < VIZ_BARS; b++)
        meter_step(&bars[b], target[b]);
}

float viz_level(int bar)
{
    return bars[bar].level;
}

float viz_peak(int bar)
{
    return bars[bar].peak;
}
