#pragma once

#include <assert.h>

#if 0
// NO OPTIMIZATIONS MODE, FOR TESTING ONLY
#elif (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
// ==== SSE2 ====
#define PPSIMD_SSE

#include <emmintrin.h>

#if defined(__SSE3__) || defined(__AVX__) || (defined(_M_IX86_FP) && _M_IX86_FP > 2)
#define PPSIMD_ALLOW_SSE3 1
#include <pmmintrin.h>
#else
#define PPSIMD_ALLOW_SSE3 0
#endif

#elif defined(__aarch64__)
// ==== ARM Neon ====

#define PPSIMD_NEON
#define PPSIMD_NEON64
#include <arm_neon.h>

#elif defined(__ARM_NEON__)

#define PPSIMD_NEON
#include <arm_neon.h>

#endif

namespace ppsimd {
#ifdef PPSIMD_SSE
    // ======================================================
    // SSE CODE
    // ======================================================
#define PPSIMD_FLOAT32_NATIVE
#define PPSIMD_FLOAT64_NATIVE
    typedef __m128 float32x4;
    typedef __m128d float64x2;

    inline float64x2 pmul(float64x2 f1, float64x2 f2) { return _mm_mul_pd(f1, f2); }
    inline float64x2 padd(float64x2 f1, float64x2 f2) { return _mm_add_pd(f1, f2); }
    inline float64x2 psub(float64x2 f1, float64x2 f2) { return _mm_sub_pd(f1, f2); }
    inline float64x2 ploadf64x2(const double* from) { return _mm_loadu_pd(from); }
    inline float64x2 ploadf64x2a(const double* from) { return _mm_load_pd(from); }
    inline float64x2 ploadf64x2(double lo, double hi) { return _mm_set_pd(hi, lo); };
    inline double pstorelo(float64x2 v) { return _mm_cvtsd_f64(v); }
#ifdef _MSC_VER
    inline double pstorehi(float64x2 v) { return v.m128d_f64[1]; }
#else
    inline double pstorehi(float64x2 v) { return v[1]; }
#endif
    inline float64x2 punplo(float64x2 v) { return _mm_unpacklo_pd(v, v); }
    inline float64x2 punphi(float64x2 v) { return _mm_unpackhi_pd(v, v); }
    inline float64x2 pswap(float64x2 f) { return _mm_shuffle_pd(f, f, _MM_SHUFFLE2(0, 1)); };
    inline void pstore(double* to, float64x2 f) { _mm_storeu_pd(to, f); }

    inline float64x2 pneg(float64x2 v) {
        return _mm_xor_pd(v, _mm_castsi128_pd(_mm_set_epi64x(0x8000000000000000ULL, 0x8000000000000000ULL)));
    }
    inline float64x2 pneghi(float64x2 v) {
        return _mm_xor_pd(v, _mm_castsi128_pd(_mm_set_epi64x(0x8000000000000000ULL, 0x0000000000000000ULL)));
    }
    inline float64x2 pneglo(float64x2 v) {
        return _mm_xor_pd(v, _mm_castsi128_pd(_mm_set_epi64x(0x0000000000000000ULL, 0x8000000000000000ULL)));
    }

    inline float64x2 paddsub(float64x2 v1, float64x2 v2) {
#if 0 // not faster
        return pswap(_mm_addsub_pd(pswap(v1), pswap(v2)));
#else
        return padd(v1, pneghi(v2));
#endif
    }
    inline float64x2 psubadd(float64x2 v1, float64x2 v2) {
#if PPSIMD_ALLOW_SSE3
        // Poor naming on Intel's side, addsub subtracts low adds high
        return _mm_addsub_pd(v1, v2);
#else
        return padd(v1, pneglo(v2));
#endif
    }

    inline double paddelems(float64x2 v1) {
#if PPSIMD_ALLOW_SSE3
        return pstorelo(_mm_hadd_pd(v1, v1));
#else
        return pstorelo(v1) + pstorehi(v1);
#endif
    }

    inline float32x4 pmul(float32x4 f1, float32x4 f2) { return _mm_mul_ps(f1, f2); }
    inline float32x4 padd(float32x4 f1, float32x4 f2) { return _mm_add_ps(f1, f2); }
    inline float32x4 psub(float32x4 f1, float32x4 f2) { return _mm_sub_ps(f1, f2); }
    inline float32x4 ploadf32x4(const float* from) { return _mm_loadu_ps(from); }
    inline float32x4 ploadf32x4a(const float* from) { return _mm_load_ps(from); }
    inline float32x4 ploadf32x4(float arg1, float arg2, float arg3, float arg4) { return _mm_set_ps(arg4, arg3, arg2, arg1); }
    inline void pstore(float* to, float32x4 f) { _mm_storeu_ps(to, f); }
    inline float32x4 pneg(float32x4 v) {
        return _mm_xor_ps(v, _mm_castsi128_ps(_mm_set_epi32(0x80000000, 0x80000000, 0x80000000, 0x80000000)));
    }
    inline float32x4 pswap(float32x4 v) {
        return _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 0, 3, 2));
    }
    inline float32x4 preverse(float32x4 v) {
        return _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 1, 2, 3));
    }
    inline bool pequals(float64x2 v1, float64x2 v2) {
        return _mm_movemask_pd( _mm_cmpeq_pd(v1, v2) ) == 0x3;
    }
    inline bool pequals(float32x4 v1, float32x4 v2) {
        return _mm_movemask_ps( _mm_cmpeq_ps(v1, v2) ) == 0xF;
    }
    inline float64x2 pzerof64x2() { return _mm_setzero_pd(); }
    inline float32x4 pzerof32x4() { return _mm_setzero_ps(); }
    inline float64x2 pset1f64x2(double v) { return _mm_set1_pd(v); }
    inline float32x4 pset1f32x4(float v) { return _mm_set1_ps(v); }

    inline float pstorelo(float32x4 f) { return _mm_cvtss_f32(f); }

    inline float pstoren(float32x4 f, int which) {
#ifdef _MSC_VER
        return f.m128_f32[which];
#else
        return f[which];
#endif
    }

    inline float paddelems(float32x4 v) {
#if PPSIMD_ALLOW_SSE3
        auto temp = _mm_hadd_ps(v, v);
        temp = _mm_hadd_ps(temp, temp);
        return pstorelo(temp);
#else
        return pstorelo(v) + pstoren(v, 1) + pstoren(v, 2) + pstoren(v, 3);
#endif
    }
    // ======================================================
    // END SSE CODE
    // ======================================================
#endif

#ifdef PPSIMD_NEON64
    // ======================================================
    // ARM64 NEON CODE
    // ======================================================
    typedef ::float64x2_t float64x2;
#define PPSIMD_FLOAT64_NATIVE

    inline float64x2 pmul(float64x2 f1, float64x2 f2) { return vmulq_f64(f1, f2); }
    inline float64x2 padd(float64x2 f1, float64x2 f2) { return vaddq_f64(f1, f2); }
    inline float64x2 psub(float64x2 f1, float64x2 f2) { return vsubq_f64(f1, f2); }

    inline float64x2 ploadf64x2(const double* from) { return vld1q_f64(from); }
    inline float64x2 ploadf64x2a(const double* from) { return vld1q_f64(from); }
    inline float64x2 ploadf64x2(double lo, double hi) { double temp[2] = { lo, hi }; return vld1q_f64(temp); } // optimize me?
    inline float64x2 pset1f64x2(double v) { return vdupq_n_f64(v); }
    inline float64x2 pzerof64x2() { return pset1f64x2(0); }
    inline double pstorelo(float64x2 v) { return v[0]; }
    inline double pstorehi(float64x2 v) { return v[1]; }
    inline float64x2 punplo(float64x2 v) { return pset1f64x2( pstorelo(v)); } // optimize me?
    inline float64x2 punphi(float64x2 v) { return pset1f64x2( pstorehi(v)); } // optimize me?
    inline float64x2 pswap(float64x2 v) { return ploadf64x2(pstorehi(v), pstorelo(v));  } // optimize me?
    inline void pstore(double* to, float64x2 f) { vst1q_f64(to, f); }

    inline float64x2 pneg(float64x2 v) {
        uint64x2_t raw = vreinterpretq_u64_f64(v);
        raw = veorq_u64(raw, uint64x2_t{ 0x8000000000000000ULL, 0x8000000000000000ULL });
        return vreinterpretq_f64_u64(raw);
    }
    inline float64x2 pneghi(float64x2 v) {
        uint64x2_t raw = vreinterpretq_u64_f64(v);
        raw = veorq_u64(raw, uint64x2_t{ 0x0000000000000000ULL, 0x8000000000000000ULL });
        return vreinterpretq_f64_u64(raw);
    }
    inline float64x2 pneglo(float64x2 v) {
        uint64x2_t raw = vreinterpretq_u64_f64(v);
        raw = veorq_u64(raw, uint64x2_t{ 0x8000000000000000ULL, 0x0000000000000000ULL });
        return vreinterpretq_f64_u64(raw);
    }

    inline float64x2 paddsub(float64x2 v1, float64x2 v2) { return padd(v1, pneghi(v2)); }
    inline float64x2 psubadd(float64x2 v1, float64x2 v2) { return padd(v1, pneglo(v2)); }

    inline bool pequals(float64x2 v1, float64x2 v2) {
        auto c = vceqq_f64(v1, v2);
        return (c[0] & c[1]) != 0;
    }
    inline double paddelems(float64x2 v) { return v[0] + v[1]; }

    // ======================================================
    // END ARM64 NEON CODE
    // ======================================================
#endif

#ifdef PPSIMD_NEON
    // ======================================================
    // ARM NEON CODE
    // ======================================================
    typedef ::float32x4_t float32x4;
#define PPSIMD_FLOAT32_NATIVE


    inline bool pequals(float32x4 v1, float32x4 v2) {
        auto c = vceqq_f32(v1, v2);
        return (c[0] & c[1] & c[2] & c[3]) != 0;
    }

    inline float32x4 pmul(float32x4 f1, float32x4 f2) { return vmulq_f32(f1,f2);}
    inline float32x4 padd(float32x4 f1, float32x4 f2) { return vaddq_f32(f1,f2);}
    inline float32x4 psub(float32x4 f1, float32x4 f2) { return vsubq_f32(f1,f2);}

    inline float32x4 ploadf32x4(const float * from ) { return vld1q_f32(from); }
    inline float32x4 ploadf32x4a(const float * from ) { return vld1q_f32(from); }
    inline float32x4 ploadf32x4(float v1, float v2, float v3, float v4) { float temp[4] = {v1, v2, v3, v4}; return vld1q_f32(temp); } // optimize me?
    inline float32x4 pset1f32x4(float v) { return vdupq_n_f32(v); }
    inline float32x4 pzerof32x4() { return pset1f32x4(0); }

    inline float pstorelo(float32x4 f) { return f[0]; }
    inline float pstoren(float32x4 f, int which) { return f[which]; }

    inline float32x4 pneg(float32x4 v) {
        uint32x4_t raw = vreinterpretq_u32_f32(v);
        raw = veorq_u32(raw, uint32x4_t{ 0x80000000, 0x80000000, 0x80000000, 0x80000000 });
        return vreinterpretq_f32_u32(raw);
    }

    inline float paddelems(float32x4 v) { return v[0] + v[1] + v[2] + v[3]; }
    inline float32x4 pswap(float32x4 v) { return ploadf32x4( v[2], v[3], v[0], v[1] ); }; // optimize me?
    inline float32x4 preverse(float32x4 v) { return ploadf32x4( v[3], v[2], v[1], v[0] ); }; // optimize me?

    // ======================================================
    // END ARM NEON CODE
    // ======================================================
#endif

#ifndef PPSIMD_FLOAT32_NATIVE
    // ======================================================
    // GENERIC CODE
    // ======================================================
    struct float32x4 { float v[4]; };

    inline float32x4 pmul(float32x4 f1, float32x4 f2) { return { f1.v[0] * f2.v[0], f1.v[1] * f2.v[1], f1.v[2] * f2.v[2], f1.v[3] * f2.v[3] }; }
    inline float32x4 padd(float32x4 f1, float32x4 f2) { return { f1.v[0] + f2.v[0], f1.v[1] + f2.v[1], f1.v[2] + f2.v[2], f1.v[3] + f2.v[3] }; }
    inline float32x4 psub(float32x4 f1, float32x4 f2) { return { f1.v[0] - f2.v[0], f1.v[1] - f2.v[1], f1.v[2] - f2.v[2], f1.v[3] - f2.v[3] }; }
    inline float32x4 ploadf32x4(const float* from) { return { from[0], from[1], from[2], from[3] }; }
    inline float32x4 ploadf32x4a(const float* from) { return { from[0], from[1], from[2], from[3] }; }
    inline float32x4 ploadf32x4(float arg1, float arg2, float arg3, float arg4) { return { arg1, arg2, arg3, arg4 }; }
    inline void pstore(float* to, float32x4 f) { to[0] = f.v[0]; to[1] = f.v[1]; to[2] = f.v[2]; to[3] = f.v[3]; }
    inline float32x4 pneg(float32x4 v) { return { -v.v[0], -v.v[1], -v.v[2], -v.v[3] }; }
    inline float32x4 pswap(float32x4 f) { return { f.v[2], f.v[3], f.v[0], f.v[1] }; };
    inline float32x4 preverse(float32x4 f) { return { f.v[3], f.v[2], f.v[1], f.v[0] }; };

    inline bool pequals(float32x4 v1, float32x4 v2) { return v1.v[0] == v2.v[0] && v1.v[1] == v2.v[1] && v1.v[2] == v2.v[2] && v1.v[3] == v2.v[3]; }

    inline float32x4 pzerof32x4() { return {}; }


    inline float32x4 pset1f32x4(float v) { return { v,v,v,v }; }

    inline float pstorelo(float32x4 f) { return f.v[0]; }
    inline float pstoren(float32x4 f, int which) { return f.v[which]; }


    inline float paddelems(float32x4 v) { return v.v[0] + v.v[1] + v.v[2] + v.v[3]; }

#endif

#ifndef PPSIMD_FLOAT64_NATIVE
    // ======================================================
    // GENERIC CODE
    // ======================================================
    struct float64x2 { double v[2]; };

    inline float64x2 pmul(float64x2 f1, float64x2 f2) { return { f1.v[0] * f2.v[0], f1.v[1] * f2.v[1] }; }
    inline float64x2 padd(float64x2 f1, float64x2 f2) { return { f1.v[0] + f2.v[0], f1.v[1] + f2.v[1] }; }
    inline float64x2 psub(float64x2 f1, float64x2 f2) { return { f1.v[0] - f2.v[0], f1.v[1] - f2.v[1] }; }
    inline float64x2 ploadf64x2(const double* from) { return { from[0], from[1] }; }
    inline float64x2 ploadf64x2a(const double* from) { return { from[0], from[1] }; }
    inline float64x2 ploadf64x2(double lo, double hi) { return { lo, hi }; }
    inline double pstorelo(float64x2 v) { return v.v[0]; }
    inline double pstorehi(float64x2 v) { return v.v[1]; }
    inline float64x2 punplo(float64x2 v) { return { v.v[0], v.v[0] }; }
    inline float64x2 punphi(float64x2 v) { return { v.v[1], v.v[1] }; }
    inline float64x2 pswap(float64x2 f) { return { f.v[1], f.v[0] }; };
    inline void pstore(double* to, float64x2 f) { to[0] = f.v[0]; to[1] = f.v[1]; }

    inline float64x2 paddsub(float64x2 v1, float64x2 v2) { return { v1.v[0] + v2.v[0], v1.v[1] - v2.v[1] }; }
    inline float64x2 psubadd(float64x2 v1, float64x2 v2) { return { v1.v[0] - v2.v[0], v1.v[1] + v2.v[1] }; }
    inline float64x2 pneg(float64x2 v) { return { -v.v[0], -v.v[1] }; }
    inline float64x2 pneghi(float64x2 v) { return { v.v[0], -v.v[1] }; }
    inline float64x2 pneglo(float64x2 v) { return { -v.v[0], v.v[1] }; }

    inline bool pequals(float64x2 v1, float64x2 v2) { return v1.v[0] == v2.v[0] && v1.v[1] == v2.v[1]; }

    inline float64x2 pzerof64x2() { return {}; }
    inline float64x2 pset1f64x2(double v) { return { v,v }; }

    inline double paddelems(float64x2 v) { return v.v[0] + v.v[1]; }
#endif



    inline float64x2 pload(double v1, double v2) { return ploadf64x2(v1, v2); }
    inline float32x4 pload(float v1, float v2, float v3, float v4) { return ploadf32x4(v1, v2, v3, v4); }
    inline float64x2 pload(const double * v) { return ploadf64x2(v); }
    inline float32x4 pload(const float* v) { return ploadf32x4(v); }

    inline void selftest() {
#ifndef NDEBUG

        {
            auto v1 = ploadf64x2(1, 2);
            auto v2 = ploadf64x2(3, 4);
            assert(pstorelo(v1) == 1);
            assert(pstorehi(v1) == 2);
            assert(pstorelo(v2) == 3);
            assert(pstorehi(v2) == 4);

            assert(!pequals(v1, v2));
            assert(pequals(v1, v1));
            assert(pequals(v2, v2));

            assert(!pequals(v1, ploadf64x2(1,1)));
            assert(!pequals(v1, ploadf64x2(2,2)));

            float64x2 v3;
            v3 = padd(v1, v2);
            assert(pstorelo(v3) == 4 && pstorehi(v3) == 6);
            v3 = paddsub(v1, v2);
            assert(pstorelo(v3) == 4 && pstorehi(v3) == -2);
            v3 = psubadd(v1, v2);
            assert(pstorelo(v3) == -2 && pstorehi(v3) == 6);
            v3 = pswap(v1);
            assert(pstorelo(v3) == 2 && pstorehi(v3) == 1);
            v3 = pmul(v1, v2);
            assert(pstorelo(v3) == 3 && pstorehi(v3) == 8);
            v3 = punplo(v1);
            assert(pstorelo(v3) == 1 && pstorehi(v3) == 1);
            v3 = punphi(v1);
            assert(pstorelo(v3) == 2 && pstorehi(v3) == 2);
            v3 = pneg(v1);
            assert(pstorelo(v3) == -1 && pstorehi(v3) == -2);
            v3 = pneglo(v1);
            assert(pstorelo(v3) == -1 && pstorehi(v3) == 2);
            v3 = pneghi(v1);
            assert(pstorelo(v3) == 1 && pstorehi(v3) == -2);

            v3 = pset1f64x2(42);
            assert(pequals(v3, ploadf64x2(42, 42)));

            v3 = pzerof64x2();
            assert(pequals(v3, ploadf64x2(0, 0)));

            assert(paddelems(v1) == 3);
            assert(paddelems(v2) == 7);
        }
        {
            float32x4 v1 = ploadf32x4(1, 2, 3, 4);
            float32x4 v2 = ploadf32x4(5, 6, 7, 8);

            assert(pstorelo(v1) == 1);
            assert(pstoren(v1, 0) == 1);
            assert(pstoren(v1, 1) == 2);
            assert(pstoren(v1, 2) == 3);
            assert(pstoren(v1, 3) == 4);
            assert(!pequals(v1, v2));
            assert(pequals(v1, v1));
            assert(pequals(v2, v2));

            assert(!pequals(v1, ploadf32x4(0,2,3,4) ));
            assert(!pequals(v1, ploadf32x4(1,0,3,4) ));
            assert(!pequals(v1, ploadf32x4(1,2,0,4) ));
            assert(!pequals(v1, ploadf32x4(1,2,3,0) ));

            assert(pequals(pswap(v1), pload(3, 4, 1, 2)));
            assert(pequals(preverse(v1), pload(4, 3, 2, 1)));



            float32x4 v3 = padd(v1, v2);
            assert(pequals(v3, pload(6, 8, 10, 12)));
            v3 = pmul(v1, v2);
            assert(pequals(v3, pload(5, 12, 21, 32)));

            v3 = pneg(v1);
            assert(pequals(v3, pload(-1, -2, -3, -4)));

            v3 = pset1f32x4(42);
            assert(pequals(v3, ploadf32x4(42, 42, 42, 42)));

            v3 = pzerof32x4();
            assert(pequals(v3, ploadf32x4(0, 0, 0, 0)));

            v3 = pswap(v1);
            assert(pequals(v3, ploadf32x4(3, 4, 1, 2)));
            v3 = preverse(v1);
            assert(pequals(v3, ploadf32x4(4, 3, 2, 1)));

            assert(paddelems(v1) == 10);
        }
#endif


    }
}
