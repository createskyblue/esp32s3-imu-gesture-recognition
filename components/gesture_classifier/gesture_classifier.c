/*
 * gesture_classifier.c - pure-C gesture classifier (ESP32-ready, no external deps).
 *
 * Pipeline (all float32):
 *   1) detrend : subtract per-axis mean from each axis (interleaved -> channel-major)
 *   2) STFT    : per channel, 2 Hann windows (64 samples at offsets 0, 36),
 *                64-point real FFT, magnitude of bins 1..32, scaled by 1/16
 *   3) interleave : feature-major layout, channels interleaved: feat[f*3+c] = ch_c[f]
 *   4) linear one-vs-rest SVM scores + beta-scaled softmax
 */
#include "gesture_classifier.h"
#include "gesture_classifier_model.h"

#include <math.h>
#include <string.h>

static int g_initialized = 0;

/* STFT window start offsets within the 100-sample axis buffer */
static const int gc_stft_offsets[GC_STFT_NUM_WINDOWS] = GC_STFT_WINDOW_OFFSETS;

/* ------------------------------------------------------------------ */
/* 64-point radix-2 complex FFT (in-place, no normalization).         */
/* Input:  real samples in x_in; Output: magnitude of bins 1..32      */
/* in mag_out (32 values).                                            */
/* ------------------------------------------------------------------ */
static void fft64_mag(const float *x_in, float *mag_out)
{
    float re[GC_STFT_WINDOW_SIZE];
    float im[GC_STFT_WINDOW_SIZE];
    int i, j, k;

    /* bit-reversed copy of the real input */
    for (i = 0; i < GC_STFT_WINDOW_SIZE; i++) {
        unsigned int b = (unsigned int)i;
        b = ((b & 0xAAAAAAAAu) >> 1) | ((b & 0x55555555u) << 1);
        b = ((b & 0xCCCCCCCCu) >> 2) | ((b & 0x33333333u) << 2);
        b = ((b & 0xF0F0F0F0u) >> 4) | ((b & 0x0F0F0F0Fu) << 4);
        j = (int)((b >> 2) & 0x3Fu); /* 8-bit reversal; low 6 bits are the reversed index */
        re[j] = x_in[i];
        im[j] = 0.0f;
    }

    /* iterative radix-2 FFT, twiddles from the precomputed table */
    for (int len = 2; len <= GC_STFT_WINDOW_SIZE; len <<= 1) {
        int half = len >> 1;
        int step = GC_STFT_WINDOW_SIZE / len;
        for (i = 0; i < GC_STFT_WINDOW_SIZE; i += len) {
            for (j = 0; j < half; j++) {
                int tw = (j * step) << 1;
                float wr = gc_twiddle64[tw];
                float wi = gc_twiddle64[tw + 1];
                k = i + j;
                int m = k + half;
                float tr = wr * re[m] - wi * im[m];
                float ti = wr * im[m] + wi * re[m];
                re[m] = re[k] - tr;
                im[m] = im[k] - ti;
                re[k] += tr;
                im[k] += ti;
            }
        }
    }

    /* magnitude of bins 1..32 (DC bin 0 is skipped), scaled by window norm */
    for (i = 1; i <= GC_STFT_WINDOW_SIZE / 2; i++) {
        mag_out[i - 1] = sqrtf(re[i] * re[i] + im[i] * im[i]) * GC_STFT_WINDOW_NORM;
    }
}

/* ------------------------------------------------------------------ */
/* Detrend: interleaved input (n samples x c channels) ->              */
/* channel-major output (c x n), per-channel mean removed.             */
/* ------------------------------------------------------------------ */
static void detrend_interleaved(const float *in, float *out, int n, int c)
{
    float mean[GC_INPUT_AXIS_NUMBER];
    int i, ch;

    for (ch = 0; ch < c; ch++) {
        float s = 0.0f;
        for (i = 0; i < n; i++) {
            s += in[i * c + ch];
        }
        mean[ch] = s / (float)n;
    }
    for (ch = 0; ch < c; ch++) {
        for (i = 0; i < n; i++) {
            out[ch * n + i] = in[i * c + ch] - mean[ch];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Feature extraction: 300 floats in -> 192 features out.              */
/* ------------------------------------------------------------------ */
static void extract_features(const float *in, float *feats)
{
    float det[GC_INPUT_SIGNAL_LENGTH * GC_INPUT_AXIS_NUMBER];
    /* per-channel features: GC_STFT_NUM_WINDOWS x 32 = 64 each */
    float ch_feats[GC_INPUT_AXIS_NUMBER][GC_STFT_NUM_WINDOWS * GC_STFT_FEATURES_PER_WINDOW];
    float win[GC_STFT_WINDOW_SIZE];
    float mag[GC_STFT_WINDOW_SIZE / 2];
    int ch, w, i, f, c;

    detrend_interleaved(in, det, GC_INPUT_SIGNAL_LENGTH, GC_INPUT_AXIS_NUMBER);

    for (ch = 0; ch < GC_INPUT_AXIS_NUMBER; ch++) {
        const float *x = &det[ch * GC_INPUT_SIGNAL_LENGTH];
        for (w = 0; w < GC_STFT_NUM_WINDOWS; w++) {
            const int start = gc_stft_offsets[w];
            for (i = 0; i < GC_STFT_WINDOW_SIZE; i++) {
                win[i] = x[start + i] * gc_hann_window[i];
            }
            fft64_mag(win, mag);
            for (i = 0; i < GC_STFT_FEATURES_PER_WINDOW; i++) {
                ch_feats[ch][w * GC_STFT_FEATURES_PER_WINDOW + i] = mag[i];
            }
        }
    }

    /* interleave: feature-major, channels interleaved */
    for (f = 0; f < GC_STFT_NUM_WINDOWS * GC_STFT_FEATURES_PER_WINDOW; f++) {
        for (c = 0; c < GC_INPUT_AXIS_NUMBER; c++) {
            feats[f * GC_INPUT_AXIS_NUMBER + c] = ch_feats[c][f];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Linear one-vs-rest SVM scores.                                      */
/* ------------------------------------------------------------------ */
static void svm_scores(const float *feats, float *scores)
{
    int k, i;
    for (k = 0; k < GC_NUMBER_OF_CLASSES; k++) {
        const float *w = &gc_svm_coefficients[k * GC_FEATURE_SIZE];
        float s = gc_svm_intercepts[k];
        for (i = 0; i < GC_FEATURE_SIZE; i++) {
            s += w[i] * feats[i];
        }
        scores[k] = s;
    }
}

/* ------------------------------------------------------------------ */
/* Beta-scaled softmax + argmax.                                       */
/* ------------------------------------------------------------------ */
static int softmax_argmax(const float *scores, float *probs)
{
    float z[GC_NUMBER_OF_CLASSES];
    float maxv = -1.0e30f;
    float sum = 0.0f;
    int i, best = 0;

    for (i = 0; i < GC_NUMBER_OF_CLASSES; i++) {
        z[i] = GC_SVM_BETA * scores[i];
        if (z[i] > maxv) {
            maxv = z[i];
            best = i;
        }
    }
    for (i = 0; i < GC_NUMBER_OF_CLASSES; i++) {
        z[i] = expf(z[i] - maxv);
        sum += z[i];
    }
    for (i = 0; i < GC_NUMBER_OF_CLASSES; i++) {
        probs[i] = z[i] / sum;
    }
    return best;
}

/* ======================= public API ======================= */

enum gc_state gc_init(void)
{
    /* Embedded model sanity check */
    if (GC_SVM_BETA <= 0.0f ||
        gc_stft_offsets[0] < 0 ||
        gc_stft_offsets[GC_STFT_NUM_WINDOWS - 1] + GC_STFT_WINDOW_SIZE > GC_INPUT_SIGNAL_LENGTH) {
        return GC_ERROR;
    }
    g_initialized = 1;
    return GC_OK;
}

enum gc_state gc_extract_features(float *in, float *features)
{
    if (!in || !features) {
        return GC_INVALID_PARAM;
    }
    extract_features(in, features);
    return GC_OK;
}

enum gc_state gc_classification(float *in, float *probabilities, int *id_class)
{
    float feats[GC_FEATURE_SIZE];
    float scores[GC_NUMBER_OF_CLASSES];

    if (!g_initialized) {
        return GC_NOT_INITIALIZED;
    }
    if (!in || !probabilities || !id_class) {
        return GC_INVALID_PARAM;
    }

    extract_features(in, feats);
    svm_scores(feats, scores);
    *id_class = softmax_argmax(scores, probabilities);
    return GC_OK;
}

int gc_get_input_signal_size(void)
{
    return GC_INPUT_SIGNAL_LENGTH;
}

int gc_get_axis_number(void)
{
    return GC_INPUT_AXIS_NUMBER;
}

int gc_get_number_of_classes(void)
{
    return GC_NUMBER_OF_CLASSES;
}

int gc_get_feature_size(void)
{
    return GC_FEATURE_SIZE;
}

const char *gc_get_class_name(int id_class)
{
    if (id_class < 0 || id_class >= GC_NUMBER_OF_CLASSES) {
        return (const char *)0;
    }
    return gc_class_names[id_class];
}