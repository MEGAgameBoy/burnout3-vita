/*
 * The few SSE operations the VU code is written in (vu_math.h, vu_core.h,
 * vu0.cpp), on ARMv7 with NEON - the Vita's Cortex-A9 - with SSE's results
 * to the bit, so the four-lanes-at-once paths the PC port was checked with
 * (VuLanesAreTheModel, VuCompiledIsTheInterpreter, the scripted runs) are the
 * same code here.
 *
 * Integer lanes, masks and shuffles are NEON. Float arithmetic is not: NEON's
 * always rounds to nearest, and the VUs round toward zero (vu.cpp's
 * VuRounding). So _mm_add_ps, _mm_sub_ps and _mm_mul_ps are four VFP
 * operations, which round as FPSCR says - the registers are shared, no lane
 * leaves them. _mm_getcsr and _mm_setcsr read and write FPSCR in MXCSR's
 * terms: the rounding control, and flush-to-zero with denormals-are-zero as
 * FPSCR's one FZ bit.
 *
 * Only what the VU code uses is here, each with the x86 semantics it relies
 * on (_mm_cvttps_epi32's 0x80000000 for what does not fit, for one).
 */

#ifndef PS2RT_SSE_ON_NEON_H
#define PS2RT_SSE_ON_NEON_H

#include <arm_neon.h>

#include <cstdint>

typedef int32x4_t __m128i;
typedef float32x4_t __m128;

#define PS2RT_NEON_INLINE static inline __attribute__((always_inline))

// ------------------------------------------------------------ loads, stores, sets

PS2RT_NEON_INLINE __m128i _mm_loadu_si128(const __m128i* p) {
    return vld1q_s32(reinterpret_cast<const int32_t*>(p));
}
PS2RT_NEON_INLINE void _mm_storeu_si128(__m128i* p, __m128i v) { vst1q_s32(reinterpret_cast<int32_t*>(p), v); }
PS2RT_NEON_INLINE __m128i _mm_set1_epi32(int x) { return vdupq_n_s32(x); }
PS2RT_NEON_INLINE __m128 _mm_set1_ps(float x) { return vdupq_n_f32(x); }
PS2RT_NEON_INLINE __m128i _mm_setzero_si128() { return vdupq_n_s32(0); }
/* Lane 0 is the last argument, as SSE's. */
PS2RT_NEON_INLINE __m128i _mm_set_epi32(int e3, int e2, int e1, int e0) {
    const int32_t lanes[4] = {e0, e1, e2, e3};
    return vld1q_s32(lanes);
}
PS2RT_NEON_INLINE __m128 _mm_castsi128_ps(__m128i v) { return vreinterpretq_f32_s32(v); }
PS2RT_NEON_INLINE __m128i _mm_castps_si128(__m128 v) { return vreinterpretq_s32_f32(v); }

// ------------------------------------------------------------ integer lanes

PS2RT_NEON_INLINE __m128i _mm_and_si128(__m128i a, __m128i b) { return vandq_s32(a, b); }
PS2RT_NEON_INLINE __m128i _mm_or_si128(__m128i a, __m128i b) { return vorrq_s32(a, b); }
PS2RT_NEON_INLINE __m128i _mm_xor_si128(__m128i a, __m128i b) { return veorq_s32(a, b); }
/* ~a & b */
PS2RT_NEON_INLINE __m128i _mm_andnot_si128(__m128i a, __m128i b) { return vbicq_s32(b, a); }
PS2RT_NEON_INLINE __m128i _mm_sub_epi32(__m128i a, __m128i b) { return vsubq_s32(a, b); }
PS2RT_NEON_INLINE __m128i _mm_srai_epi32(__m128i a, int count) {
    return vshlq_s32(a, vdupq_n_s32(-(count > 31 ? 31 : count)));
}
PS2RT_NEON_INLINE __m128i _mm_cmpeq_epi32(__m128i a, __m128i b) { return vreinterpretq_s32_u32(vceqq_s32(a, b)); }
PS2RT_NEON_INLINE __m128i _mm_cmpgt_epi32(__m128i a, __m128i b) { return vreinterpretq_s32_u32(vcgtq_s32(a, b)); }
PS2RT_NEON_INLINE __m128i _mm_cmplt_epi32(__m128i a, __m128i b) { return vreinterpretq_s32_u32(vcltq_s32(a, b)); }
PS2RT_NEON_INLINE __m128i _mm_min_epu32(__m128i a, __m128i b) {
    return vreinterpretq_s32_u32(vminq_u32(vreinterpretq_u32_s32(a), vreinterpretq_u32_s32(b)));
}

/* Lane i of the result is a's lane (imm >> 2i) & 3. */
template <int Imm>
PS2RT_NEON_INLINE __m128i ps2rt_neon_shuffle_epi32(__m128i a) {
    return __builtin_shuffle(a, (int32x4_t){Imm & 3, (Imm >> 2) & 3, (Imm >> 4) & 3, (Imm >> 6) & 3});
}
#define _mm_shuffle_epi32(a, imm) ps2rt_neon_shuffle_epi32<(imm)>(a)
#ifndef _MM_SHUFFLE
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#endif

/* Each byte's top bit, byte 0 the lowest bit of the 16. */
PS2RT_NEON_INLINE int _mm_movemask_epi8(__m128i v) {
    const uint8x16_t top = vshrq_n_u8(vreinterpretq_u8_s32(v), 7);
    // Weigh each byte's bit by its place within its half, then add the halves up.
    static const uint8_t kWeights[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t weighted = vmulq_u8(top, vld1q_u8(kWeights));
    const uint8x8_t low = vget_low_u8(weighted), high = vget_high_u8(weighted);
    const uint8x8_t sum = vpadd_u8(vpadd_u8(vpadd_u8(low, high), vdup_n_u8(0)), vdup_n_u8(0));
    return int(vget_lane_u8(sum, 0)) | int(vget_lane_u8(sum, 1)) << 8;
}

/* Each lane's sign, lane 0 the lowest of the 4 bits. */
PS2RT_NEON_INLINE int _mm_movemask_ps(__m128 v) {
    const uint32x4_t top = vshrq_n_u32(vreinterpretq_u32_f32(v), 31);
    static const int32_t kShifts[4] = {0, 1, 2, 3};
    const uint32x4_t placed = vshlq_u32(top, vld1q_s32(kShifts));
    const uint32x2_t pairs = vpadd_u32(vget_low_u32(placed), vget_high_u32(placed));
    return int(vget_lane_u32(vpadd_u32(pairs, pairs), 0));
}

// ------------------------------------------------------------ float lanes, rounded as FPSCR says

/* (The compiler keeps these VFP: it never turns float arithmetic into NEON's
   without -funsafe-math-optimizations, which nothing here is built with.) */
#define PS2RT_NEON_VFP_LANES(name, op)                                                       \
    PS2RT_NEON_INLINE __m128 name(__m128 a, __m128 b) {                                     \
        const float32x4_t r = {vgetq_lane_f32(a, 0) op vgetq_lane_f32(b, 0),                \
                               vgetq_lane_f32(a, 1) op vgetq_lane_f32(b, 1),                \
                               vgetq_lane_f32(a, 2) op vgetq_lane_f32(b, 2),                \
                               vgetq_lane_f32(a, 3) op vgetq_lane_f32(b, 3)};               \
        return r;                                                                           \
    }
PS2RT_NEON_VFP_LANES(_mm_add_ps, +)
PS2RT_NEON_VFP_LANES(_mm_sub_ps, -)
PS2RT_NEON_VFP_LANES(_mm_mul_ps, *)
#undef PS2RT_NEON_VFP_LANES

/* a >= b, all ones where so (no rounding: NEON's compare is exact; a
   denormal compares as zero, as SSE's does under DAZ). */
PS2RT_NEON_INLINE __m128 _mm_cmpge_ps(__m128 a, __m128 b) { return vreinterpretq_f32_u32(vcgeq_f32(a, b)); }

/* Truncated to integers; anything that does not fit (and a NaN) is
   0x80000000, as SSE's "integer indefinite" - NEON's would saturate. */
PS2RT_NEON_INLINE __m128i _mm_cvttps_epi32(__m128 a) {
    const int32x4_t value = vcvtq_s32_f32(a);
    const uint32x4_t fits = vandq_u32(vcltq_f32(a, vdupq_n_f32(2147483648.0f)),
                                      vcgeq_f32(a, vdupq_n_f32(-2147483648.0f)));   // false for a NaN
    return vbslq_s32(fits, value, vdupq_n_s32(INT32_MIN));
}

// ------------------------------------------------------------ the rounding mode, as MXCSR

PS2RT_NEON_INLINE unsigned ps2rt_read_fpscr() {
    unsigned value;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(value) : : "memory");
    return value;
}
PS2RT_NEON_INLINE void ps2rt_write_fpscr(unsigned value) {
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(value) : "memory");
}

/* MXCSR's rounding control (bits 13-14: nearest, down, up, toward zero) and
   FTZ (15) / DAZ (6), from FPSCR's RMode (22-23: nearest, up, down, toward
   zero) and FZ (24). The exception masks read as all masked. */
PS2RT_NEON_INLINE unsigned _mm_getcsr() {
    const unsigned fpscr = ps2rt_read_fpscr();
    static const unsigned kRc[4] = {0u, 2u, 1u, 3u};     // RMode -> RC
    unsigned mxcsr = 0x1F80u | kRc[(fpscr >> 22) & 3] << 13;
    if (fpscr & (1u << 24)) mxcsr |= 0x8000u | 0x0040u;
    return mxcsr;
}
PS2RT_NEON_INLINE void _mm_setcsr(unsigned mxcsr) {
    static const unsigned kRMode[4] = {0u, 2u, 1u, 3u};  // RC -> RMode
    unsigned fpscr = ps2rt_read_fpscr() & ~((3u << 22) | (1u << 24));
    fpscr |= kRMode[(mxcsr >> 13) & 3] << 22;
    if (mxcsr & 0x8000u) fpscr |= 1u << 24;
    ps2rt_write_fpscr(fpscr);
}

#undef PS2RT_NEON_INLINE

#endif  /* PS2RT_SSE_ON_NEON_H */
