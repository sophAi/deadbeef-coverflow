/* Copyright  (C) 2010-2018 The RetroArch team
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

 /* Modified by Janne Hyvärinen */
 /* Modified some more by Peter Pawlowski */

#pragma once

#define __forceinline

#include "ppsimd/ppsimd.h"
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#if !__APPLE__
#include <malloc.h>
#endif

#ifdef _MSC_VER
#define _NOALIAS __declspec(noalias)
#define _RESTRICT __declspec(restrict)
#endif

#ifndef _NOALIAS
#define _NOALIAS
#endif
#ifndef _RESTRICT
#define _RESTRICT
#endif

/* API definitions */

enum resampler_quality {            /* Rough SNR values for upsampling: */
    RESAMPLER_QUALITY_LOWEST = 0,   /* LOWEST:   40 dB                  */
    RESAMPLER_QUALITY_LOWER,        /* LOWER:    55 dB                  */
    RESAMPLER_QUALITY_NORMAL,       /* NORMAL:   70 dB                  */
    RESAMPLER_QUALITY_HIGHER,       /* HIGHER:  110 dB                  */
    RESAMPLER_QUALITY_HIGHEST       /* HIGHEST: 140 dB                  */
};

struct resampler_data {
    const float *data_in;
    float *data_out;

    size_t input_frames;
    size_t output_frames;
};

static bool resampler_sinc_ratio_supported(unsigned int srate_source, unsigned int srate_target, enum resampler_quality quality);
/* returns pointer to rarch_sinc_resampler_t */
static void *resampler_sinc_new(unsigned int srate_source, unsigned int srate_target, unsigned int num_channels, enum resampler_quality quality);
static void resampler_sinc_flush(void *data);
static void resampler_sinc_free(void *data);

/* end of API definitions */

typedef void (*resampler_sinc_process_t)(void *re_, struct resampler_data *data);

enum class sinc_window {
    NONE = 0,
    KAISER,
    LANCZOS
};

typedef struct rarch_sinc_resampler {
    resampler_sinc_process_t process;
    unsigned int num_channels;
    double ratio;
    unsigned int phase_bits;
    unsigned int subphase_bits;
    unsigned int subphase_mask;
    unsigned int taps;
    unsigned int ptr;
    unsigned int skip;
    unsigned int initial_skip;
    uint32_t time;
    float subphase_mod;
    float kaiser_beta;
    enum sinc_window window_type;

    /* A buffer for phase_table, buffer_l and buffer_r
    * are created in a single calloc().
    * Ensure that we get as good cache locality as we can hope for. */
    float *main_buffer;
    float *phase_table;
    float *buffer_l; /* left channel or beginning of channel buffer when num_channels > 2 */
    float *buffer_r;
} rarch_sinc_resampler_t;

struct resampler_quality_settings {
    double cutoff;
    unsigned int sidelobes;
    unsigned int phase_bits;
    unsigned int subphase_bits;
    float kaiser_beta;
    enum sinc_window window_type;
};

static const resampler_quality_settings resampler_quality_levels[5] = {
    { 0.980,   2, 12, 10,  0.0f, sinc_window::LANCZOS }, /* RESAMPLER_QUALITY_LOWEST  /*/
    { 0.980,   4, 12, 10,  0.0f, sinc_window::LANCZOS }, /* RESAMPLER_QUALITY_LOWER   /*/
    { 0.825,   8,  8, 16,  5.5f, sinc_window::KAISER  }, /* RESAMPLER_QUALITY_NORMAL  /*/
    { 0.900,  32, 10, 14, 10.5f, sinc_window::KAISER  }, /* RESAMPLER_QUALITY_HIGHER  /*/
    { 0.962, 128, 10, 14, 14.5f, sinc_window::KAISER  }  /* RESAMPLER_QUALITY_HIGHEST /*/
};

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327
#endif

static __forceinline double sinc(double val)
{
    if (fabs(val) < 0.00001) return 1.0;
    return sin(val) / val;
}

static __forceinline double besseli0(double x)
{
    unsigned i;
    double sum            = 0.0;
    double factorial      = 1.0;
    double factorial_mult = 0.0;
    double x_pow          = 1.0;
    double two_div_pow    = 1.0;
    double x_sqr          = x * x;

    /* Approximate. This is an infinite sum.
    * Luckily, it converges rather fast. */
    for (i = 0; i < 18; i++) {
        sum += x_pow * two_div_pow / (factorial * factorial);

        factorial_mult += 1.0;
        x_pow *= x_sqr;
        two_div_pow *= 0.25;
        factorial *= factorial_mult;
    }

    return sum;
}

static __forceinline double kaiser_window_function(double index, double beta)
{
    return besseli0(beta * sqrtf((float)(1 - (index * index))));
}

static __forceinline double lanzcos_window_function(double index)
{
    return sinc(M_PI * index);
}

/* sinc resampler */
template<bool bKaiser>
static _NOALIAS void resampler_sinc_process_simd_stereo(void *re_, struct resampler_data *data)
{
    rarch_sinc_resampler_t *resamp = (rarch_sinc_resampler_t*)re_;
    unsigned phases                = 1 << (resamp->phase_bits + resamp->subphase_bits);

    uint32_t ratio                 = (uint32_t)(phases / resamp->ratio + 0.5);
    const float *input             = data->data_in;
    float *output                  = data->data_out;
    size_t frames                  = data->input_frames;
    size_t out_frames              = 0;

    while (frames) {
        while (frames && resamp->time >= phases) {
            /* Push in reverse to make filter more obvious. */
            if (!resamp->ptr) resamp->ptr = resamp->taps;
            resamp->ptr--;

            resamp->buffer_l[resamp->ptr + resamp->taps] = resamp->buffer_l[resamp->ptr] = *input++;
            resamp->buffer_r[resamp->ptr + resamp->taps] = resamp->buffer_r[resamp->ptr] = *input++;

            resamp->time -= phases;
            frames--;
        }

        while (resamp->time < phases) {

            if (resamp->skip == 0) {
                using namespace ppsimd;
                unsigned i;
                float32x4 sum_l, sum_r, delta;
                float* phase_table = NULL;
                float* delta_table = NULL;
                const float* buffer_l = resamp->buffer_l + resamp->ptr;
                const float* buffer_r = resamp->buffer_r + resamp->ptr;
                unsigned taps = resamp->taps;
                unsigned phase = resamp->time >> resamp->subphase_bits;

                if (/*resamp->window_type == sinc_window::KAISER*/ bKaiser) {
                    phase_table = resamp->phase_table + phase * taps * 2;
                    delta_table = phase_table + taps;
                    delta = pset1f32x4((float)(resamp->time & resamp->subphase_mask) * resamp->subphase_mod);
                } else {
                    phase_table = resamp->phase_table + phase * taps;
                }

                sum_r = sum_l = pzerof32x4();

                for (i = 0; i < taps; i += 4) {
                    float32x4 _sinc;
                    float32x4 buf_l = ploadf32x4(buffer_l + i);
                    float32x4 buf_r = ploadf32x4(buffer_r + i);

                    if (/*resamp->window_type == sinc_window::KAISER*/ bKaiser) {
                        auto deltas = ploadf32x4a(delta_table + i);
                        _sinc = padd(ploadf32x4((const float*)phase_table + i), pmul(deltas, delta));
                    } else {
                        _sinc = ploadf32x4a((const float*)phase_table + i);
                    }
                    sum_l = padd(sum_l, pmul(buf_l, _sinc));
                    sum_r = padd(sum_r, pmul(buf_r, _sinc));
                }


#ifdef PPSIMD_SSE
                // Original libretro SSE shuffle party
                auto sum = _mm_add_ps(_mm_shuffle_ps(sum_l, sum_r, _MM_SHUFFLE(1, 0, 1, 0)), _mm_shuffle_ps(sum_l, sum_r, _MM_SHUFFLE(3, 2, 3, 2)));
                sum = _mm_add_ps(_mm_shuffle_ps(sum, sum, _MM_SHUFFLE(3, 3, 1, 1)), sum);
                _mm_store_ss(output++, sum);
                _mm_store_ss(output++, _mm_movehl_ps(sum, sum));
#else
                * (output++) = paddelems(sum_l);
                *(output++) = paddelems(sum_r);
#endif

                out_frames++;
            } else {
                resamp->skip--;
            }
            resamp->time += ratio;
        }
    }

    data->output_frames = out_frames;
}

template<bool bKaiser>
static _NOALIAS void resampler_sinc_process_simd(void* re_, struct resampler_data* data)
{
    rarch_sinc_resampler_t* resamp = (rarch_sinc_resampler_t*)re_;
    unsigned phases = 1 << (resamp->phase_bits + resamp->subphase_bits);

    uint32_t ratio = (uint32_t)(phases / resamp->ratio + 0.5);
    const float* input = data->data_in;
    float* output = data->data_out;
    size_t frames = data->input_frames;
    unsigned int channels = resamp->num_channels;
    unsigned int taps = resamp->taps;
    size_t out_frames = 0;

    while (frames) {
        unsigned int c;
        while (frames && resamp->time >= phases) {
            /* Push in reverse to make filter more obvious. */
            if (!resamp->ptr) resamp->ptr = resamp->taps;
            resamp->ptr--;

            for (c = 0; c < channels; ++c) {
                resamp->buffer_l[(resamp->ptr + resamp->taps) + (c * 2 * resamp->taps)] = resamp->buffer_l[(resamp->ptr) + (c * 2 * resamp->taps)] = *input++;
            }

            resamp->time -= phases;
            frames--;
        }

        while (resamp->time < phases) {

            if (resamp->skip == 0) {
                using namespace ppsimd;
                unsigned i;
                float32x4 delta;
                float* phase_table, * delta_table;
                const float* buffer_l = resamp->buffer_l + resamp->ptr;
                unsigned int phase = resamp->time >> resamp->subphase_bits;
                if (bKaiser) {
                    phase_table = resamp->phase_table + phase * taps * 2;
                    delta_table = phase_table + taps;
                    delta = pset1f32x4((float)(resamp->time & resamp->subphase_mask) * resamp->subphase_mod);
                } else {
                    phase_table = resamp->phase_table + phase * taps;
                }

                for (c = 0; c < channels; c++) {

                    const float* pbuf_l = buffer_l + (c * 2 * taps);
                    float32x4 sum = pzerof32x4();

                    for (i = 0; i < taps; i += 4) {
                        auto sinc_val = ploadf32x4a(&phase_table[i]);
                        if (bKaiser) sinc_val = padd(sinc_val, pmul(ploadf32x4a(&delta_table[i]), delta));

                        sum = padd(sum, pmul(ploadf32x4(pbuf_l), sinc_val));
                        pbuf_l += 4;
                    }

                    *output++ = paddelems(sum);
                }
                out_frames++;
            } else {
                resamp->skip--;
            }

            resamp->time += ratio;
        }
    }

    data->output_frames = out_frames;
}

static _NOALIAS void resampler_sinc_process_c(void *re_, struct resampler_data *data)
{
    rarch_sinc_resampler_t *resamp = (rarch_sinc_resampler_t*)re_;
    unsigned phases                = 1 << (resamp->phase_bits + resamp->subphase_bits);

    uint32_t ratio                 = (uint32_t)(phases / resamp->ratio + 0.5);
    const float *input             = data->data_in;
    float *output                  = data->data_out;
    size_t frames                  = data->input_frames;
    unsigned int channels          = resamp->num_channels;
    unsigned int taps              = resamp->taps;
    size_t out_frames              = 0;

    while (frames) {
        unsigned int c;
        while (frames && resamp->time >= phases) {
            /* Push in reverse to make filter more obvious. */
            if (!resamp->ptr) resamp->ptr = resamp->taps;
            resamp->ptr--;

            for (c = 0; c < channels; ++c) {
                resamp->buffer_l[(resamp->ptr + resamp->taps) + (c*2*resamp->taps)] = resamp->buffer_l[(resamp->ptr) + (c*2*resamp->taps)] = *input++;
            }

            resamp->time -= phases;
            frames--;
        }

        while (resamp->time < phases) {
            unsigned i;
            float delta = 0.0;
            float *phase_table, *delta_table = nullptr;
            const float *buffer_l = resamp->buffer_l + resamp->ptr;
            unsigned int phase = resamp->time >> resamp->subphase_bits;

            if (resamp->window_type == sinc_window::KAISER) {
                phase_table = resamp->phase_table + phase * taps * 2;
                delta_table = phase_table + taps;
                delta = (float)(resamp->time & resamp->subphase_mask) * resamp->subphase_mod;
            } else {
                phase_table = resamp->phase_table + phase * taps;
            }

            for (c = 0; c < channels; c++) {
                float sum = 0.0f;

                for (i = 0; i < taps; i++) {
                    float sinc_val = phase_table[i];
                    if (resamp->window_type == sinc_window::KAISER) sinc_val += delta_table[i] * delta;
                    sum += buffer_l[i + (c*2*taps)] * sinc_val;
                }

                if (resamp->skip == 0) *output++ = sum;
            }

            if (resamp->skip == 0) {
                out_frames++;
            } else {
                resamp->skip--;
            }

            resamp->time += ratio;
        }
    }

    data->output_frames = out_frames;
}

static _NOALIAS void sinc_init_table_kaiser(rarch_sinc_resampler_t *resamp, double cutoff, float *phase_table, int phases, int taps, bool calculate_delta)
{
    int i, j;
    double window_mod = kaiser_window_function(0.0, resamp->kaiser_beta); /* Need to normalize w(0) to 1.0. */
    int stride = calculate_delta ? 2 : 1;
    double sidelobes = taps / 2.0;

    for (i = 0; i < phases; i++) {
        for (j = 0; j < taps; j++) {
            double sinc_phase;
            float val;
            int               n = j * phases + i;
            double window_phase = (double)n / (phases * taps); /* [0, 1). */
            window_phase        = 2.0 * window_phase - 1.0; /* [-1, 1) */
            sinc_phase          = sidelobes * window_phase;
            val                 = (float)(cutoff * sinc(M_PI * sinc_phase * cutoff) * kaiser_window_function(window_phase, resamp->kaiser_beta) / window_mod);
            phase_table[i * stride * taps + j] = val;
        }
    }

    if (calculate_delta) {
        int phase;
        int p;

        for (p = 0; p < phases - 1; p++) {
            for (j = 0; j < taps; j++) {
                float delta = phase_table[(p + 1) * stride * taps + j] - phase_table[p * stride * taps + j];
                phase_table[(p * stride + 1) * taps + j] = delta;
            }
        }

        phase = phases - 1;
        for (j = 0; j < taps; j++) {
             float val, delta;
             double sinc_phase;
             int n               = j * phases + (phase + 1);
             double window_phase = (double)n / (phases * taps); /* (0, 1]. */
             window_phase        = 2.0 * window_phase - 1.0; /* (-1, 1] */
             sinc_phase          = sidelobes * window_phase;

             val                 = (float)(cutoff * sinc(M_PI * sinc_phase * cutoff) * kaiser_window_function(window_phase, resamp->kaiser_beta) / window_mod);
             delta = (val - phase_table[phase * stride * taps + j]);
             phase_table[(phase * stride + 1) * taps + j] = delta;
        }
    }
}

static _NOALIAS void sinc_init_table_lanczos(rarch_sinc_resampler_t *resamp, double cutoff, float *phase_table, int phases, int taps, bool calculate_delta)
{
    int i, j;
    double window_mod = lanzcos_window_function(0.0); /* Need to normalize w(0) to 1.0. */
    int stride = calculate_delta ? 2 : 1;
    double sidelobes = taps / 2.0;

    for (i = 0; i < phases; i++) {
        for (j = 0; j < taps; j++) {
            double sinc_phase;
            float val;
            int               n = j * phases + i;
            double window_phase = (double)n / (phases * taps); /* [0, 1). */
            window_phase        = 2.0 * window_phase - 1.0; /* [-1, 1) */
            sinc_phase          = sidelobes * window_phase;
            val                 = (float)(cutoff * sinc(M_PI * sinc_phase * cutoff) * lanzcos_window_function(window_phase) / window_mod);
            phase_table[i * stride * taps + j] = val;
        }
    }

    if (calculate_delta) {
        int phase;
        int p;

        for (p = 0; p < phases - 1; p++) {
            for (j = 0; j < taps; j++) {
                float delta = phase_table[(p + 1) * stride * taps + j] - phase_table[p * stride * taps + j];
                phase_table[(p * stride + 1) * taps + j] = delta;
            }
        }

        phase = phases - 1;
        for (j = 0; j < taps; j++) {
            float val, delta;
            double sinc_phase;
            int n               = j * phases + (phase + 1);
            double window_phase = (double)n / (phases * taps); /* (0, 1]. */
            window_phase        = 2.0 * window_phase - 1.0; /* (-1, 1] */
            sinc_phase          = sidelobes * window_phase;

            val                 = (float)(cutoff * sinc(M_PI * sinc_phase * cutoff) * lanzcos_window_function(window_phase) / window_mod);
            delta = (val - phase_table[phase * stride * taps + j]);
            phase_table[(phase * stride + 1) * taps + j] = delta;
        }
    }
}

static bool resampler_sinc_ratio_supported(unsigned int srate_source, unsigned int srate_target, enum resampler_quality quality)
{
    if (srate_source > 0 && srate_target > 0) {
        double ratio;
        unsigned int taps, phase_bits, subphase_bits, phases;
        if (quality < RESAMPLER_QUALITY_LOWEST || quality > RESAMPLER_QUALITY_HIGHEST) quality = RESAMPLER_QUALITY_NORMAL;

        taps = resampler_quality_levels[quality].sidelobes * 2;
        phase_bits = resampler_quality_levels[quality].phase_bits;
        subphase_bits = resampler_quality_levels[quality].subphase_bits;

        ratio = (double)srate_target / (double)srate_source;
        if (ratio < 1.0) {
            double new_taps = ceil(taps / ratio);
            if (new_taps >= (UINT32_MAX-4)) return false;
            taps = (unsigned int)new_taps;
        }

        phases = 1 << (phase_bits + subphase_bits);
        ratio = (double)phases / ratio;
        if (ratio >= UINT32_MAX) return false;

        return true;
    }

    return false;
}

#ifdef _WIN32
static void *resampler_buffer_alloc(size_t alignment, size_t size) {
    return _aligned_malloc(size, alignment);
}
static void resampler_buffer_free(void *buffer) {
    _aligned_free (buffer);
}
#else
static void *resampler_buffer_alloc(size_t alignment, size_t size) {
    void *ptr = nullptr;
    if (0 != posix_memalign(&ptr, alignment, size)) {
        return nullptr;
    }
    return ptr;
}
static void resampler_buffer_free(void *buffer) {
    free (buffer);
}
#endif

static _NOALIAS _RESTRICT void *resampler_sinc_new(unsigned int srate_source, unsigned int srate_target, unsigned int num_channels, enum resampler_quality quality)
{
    ppsimd::selftest();
    rarch_sinc_resampler_t *re;
    double cutoff;
    size_t phase_elems, elems, i;
    if (!resampler_sinc_ratio_supported(srate_source, srate_target, quality)) return NULL;

    re = (rarch_sinc_resampler_t *)calloc(1, sizeof(*re));
    if (!re) return NULL;

    if (quality < RESAMPLER_QUALITY_LOWEST || quality > RESAMPLER_QUALITY_HIGHEST) quality = RESAMPLER_QUALITY_NORMAL;

    cutoff            = resampler_quality_levels[quality].cutoff;
    re->taps          = resampler_quality_levels[quality].sidelobes * 2;
    re->phase_bits    = resampler_quality_levels[quality].phase_bits;
    re->subphase_bits = resampler_quality_levels[quality].subphase_bits;
    re->kaiser_beta   = resampler_quality_levels[quality].kaiser_beta;
    re->window_type   = resampler_quality_levels[quality].window_type;

    re->subphase_mask = (1 << re->subphase_bits) - 1;
    re->subphase_mod  = 1.0f / (1 << re->subphase_bits);
    re->num_channels  = num_channels;
    re->ratio         = (double)srate_target / (double)srate_source;
    re->initial_skip  = re->taps / 2;
    re->skip          = re->initial_skip;

    /* Downsampling, must lower cutoff, and extend number of
     * taps accordingly to keep same stopband attenuation. */
    if (re->ratio < 1.0) {
        cutoff *= re->ratio;
        re->taps = (unsigned)ceil(re->taps / re->ratio);
    }

    /* Be SIMD-friendly. */
    re->taps = (re->taps + 3) & ~3;

    phase_elems = ((1 << re->phase_bits) * re->taps);
    if (re->window_type == sinc_window::KAISER) phase_elems *= 2;
    elems = phase_elems + (2*num_channels) * re->taps;

    re->main_buffer = (float *)resampler_buffer_alloc(128, sizeof(float) * elems);
    if (!re->main_buffer) {
        resampler_sinc_free(re);
        return NULL;
    }

    for (i = 0; i < elems; ++i) {
        re->main_buffer[i] = 0.0f;
    }

    re->phase_table = re->main_buffer;
    re->buffer_l    = re->main_buffer + phase_elems;
    if (num_channels == 2) re->buffer_r = re->buffer_l + 2 * re->taps;

    switch (re->window_type) {
    default:
    case sinc_window::LANCZOS:
        sinc_init_table_lanczos(re, cutoff, re->phase_table, 1 << re->phase_bits, re->taps, false);
        break;
    case sinc_window::KAISER:
        sinc_init_table_kaiser(re, cutoff, re->phase_table, 1 << re->phase_bits, re->taps, true);
        break;
    }
    const bool bKaiser = (re->window_type == sinc_window::KAISER);
    re->process = bKaiser ? resampler_sinc_process_simd<true> : resampler_sinc_process_simd<false>;
    if (num_channels == 2) {
        re->process = bKaiser ? resampler_sinc_process_simd_stereo<true> : resampler_sinc_process_simd_stereo<false>;
    }

    return re;
}

static _NOALIAS void resampler_sinc_flush(void *data)
{
    rarch_sinc_resampler_t *resamp = (rarch_sinc_resampler_t*)data;
    resamp->ptr = 0;
    resamp->time = 0;
    resamp->skip = resamp->initial_skip;
}

static _NOALIAS void resampler_sinc_free(void *data)
{
    rarch_sinc_resampler_t *resamp = (rarch_sinc_resampler_t*)data;
    if (resamp) resampler_buffer_free((void *)resamp->main_buffer);
    free(resamp);
}
