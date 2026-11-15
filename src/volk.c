/* Vendored VOLK kernels; see include/volk.h for source and license. */

#include "volk.h"

#include <complex.h>
#include <stdint.h>
#include <string.h>

typedef float v8 __attribute__((vector_size(32)));
typedef float v4 __attribute__((vector_size(16)));
typedef int iv8 __attribute__((vector_size(32))); // shuffle masks / lane selects

/* Upstream kernels load/store whole vectors through unaligned pointers; the
 * memcpy form lets gcc emit a single move without alignment assumptions. */
#define V8_LOAD(dst, src) memcpy(&(dst), (src), sizeof(v8))
#define V8_STORE(dst, src) memcpy((dst), &(src), sizeof(v8))

// lane-parallel reduction of a v8 (pairwise halves, then a 3-add tail)
static inline float volk_hsum8(v8 v)
{
    v4 lo, hi;
    memcpy(&lo, &v, sizeof(lo));
    memcpy(&hi, (const char *)&v + sizeof(lo), sizeof(lo));
    v4 t = lo + hi;
    return t[0] + t[1] + t[2] + t[3];
}

// Upstream: volk_32fc_x2_multiply_32fc.h, u_avx2_fma variant. Per 4 complex:
// dup the real/imag lanes of b, swap a's lane pairs, then one FMA-contracted
// multiply-add reproduces the fmaddsub lane pattern exactly.
void volk_32fc_x2_multiply_32fc(
    lv_32fc_t *cVector, const lv_32fc_t *aVector, const lv_32fc_t *bVector, unsigned int num_points)
{
    const v8 sgn = {-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f};

    unsigned int quarter = (num_points / 4) * 4;
    for (unsigned int i = 0; i < quarter; i += 4)
    {
        v8 x, y;
        V8_LOAD(x, aVector + i);
        V8_LOAD(y, bVector + i);
        v8 yl = __builtin_shufflevector(y, y, 0, 0, 2, 2, 4, 4, 6, 6); // b re-duplicated per pair
        v8 yh = __builtin_shufflevector(y, y, 1, 1, 3, 3, 5, 5, 7, 7); // b im-duplicated per pair
        v8 xs = __builtin_shufflevector(x, x, 1, 0, 3, 2, 5, 4, 7, 6); // a with lane pairs swapped
        v8 z = x * yl + sgn * (xs * yh);
        V8_STORE(cVector + i, z);
    }
    for (unsigned int i = quarter; i < num_points; i++)
        cVector[(size_t)i] = aVector[(size_t)i] * bVector[(size_t)i];
}

// Upstream: volk_32fc_magnitude_squared_32f.h, u_avx variant. Per 4 complex:
// square the interleaved pairs, add each pair via a lane swap, store the
// even lanes (one magnitude per complex).
void volk_32fc_magnitude_squared_32f(
    float *magnitudeVector, const lv_32fc_t *complexVector, unsigned int num_points)
{

    unsigned int quarter = (num_points / 4) * 4;
    for (unsigned int i = 0; i < quarter; i += 4)
    {
        v8 x;
        V8_LOAD(x, complexVector + i);
        v8 sq = x * x;
        v8 s = sq + __builtin_shufflevector(
                        sq, sq, 1, 0, 3, 2, 5, 4, 7, 6); // both lanes of a pair hold r^2+i^2
        v8 e = __builtin_shufflevector(s, s, 0, 2, 4, 6, 0, 2, 4, 6); // compact the pair sums
        memcpy(magnitudeVector + (size_t)i, &e, 4 * sizeof(float));
    }
    const float *cv = (const float *)complexVector;
    for (unsigned int i = quarter; i < num_points; i++)
        magnitudeVector[i] =
            cv[(size_t)2 * i] * cv[(size_t)2 * i] + cv[(size_t)2 * i + 1] * cv[(size_t)2 * i + 1];
}

// Upstream: volk_32fc_conjugate_32fc.h; the generic loop with a sign-mask
// vector form for the bulk.
void volk_32fc_conjugate_32fc(lv_32fc_t *cVector, const lv_32fc_t *aVector, unsigned int num_points)
{
    const v8 sgn = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};

    unsigned int quarter = (num_points / 4) * 4;
    for (unsigned int i = 0; i < quarter; i += 4)
    {
        v8 x;
        V8_LOAD(x, aVector + i);
        v8 z = x * sgn; // flip the imaginary lanes
        V8_STORE(cVector + i, z);
    }
    for (unsigned int i = quarter; i < num_points; i++)
        cVector[(size_t)i] = conjf(aVector[(size_t)i]);
}

// Upstream: volk_32f_x2_dot_prod_32f.h, generic variant vectorized.
void volk_32f_x2_dot_prod_32f(
    float *result, const float *input, const float *taps, unsigned int num_points)
{
    v8 acc = {0};
    unsigned int i = 0;
    for (; i + 8 <= num_points; i += 8)
    {
        v8 a, b;
        V8_LOAD(a, input + i);
        V8_LOAD(b, taps + i);
        acc += a * b;
    }
    float dot = volk_hsum8(acc);
    for (; i < num_points; i++)
        dot += input[(size_t)i] * taps[(size_t)i];
    *result = dot;
}

// Upstream: volk_32f_accumulator_s32f.h, generic variant vectorized.
void volk_32f_accumulator_s32f(float *result, const float *inputBuffer, unsigned int num_points)
{
    v8 acc = {0};
    unsigned int i = 0;
    for (; i + 8 <= num_points; i += 8)
    {
        v8 a;
        V8_LOAD(a, inputBuffer + i);
        acc += a;
    }
    float sum = volk_hsum8(acc);
    for (; i < num_points; i++)
        sum += inputBuffer[(size_t)i];
    *result = sum;
}

// Upstream: volk_32f_x2_add_32f.h, generic variant (verbatim; gcc
// auto-vectorizes the element-wise float loop).
void volk_32f_x2_add_32f(
    float *cVector, const float *aVector, const float *bVector, unsigned int num_points)
{
    for (unsigned int number = 0; number < num_points; number++)
        cVector[number] = aVector[number] + bVector[number];
}

// Upstream: volk_32f_x2_subtract_32f.h, generic variant (verbatim).
void volk_32f_x2_subtract_32f(
    float *cVector, const float *aVector, const float *bVector, unsigned int num_points)
{
    for (unsigned int number = 0; number < num_points; number++)
        cVector[number] = aVector[number] - bVector[number];
}

// per-lane float select: all-ones cm lanes take a's bits, zero lanes b's
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (a, b) are the blend arms
static inline v8 volk_blend8(iv8 cm, v8 a, v8 b)
{
    iv8 ab, bb, r;
    memcpy(&ab, &a, sizeof(ab));
    memcpy(&bb, &b, sizeof(bb));
    r = (ab & cm) | (bb & ~cm);
    v8 out;
    memcpy(&out, &r, sizeof(out));
    return out;
}

// Upstream: volk_32f_index_max_32u.h, sse/avx2 blend form ported: strict
// lane compare keeps the first maximum, a blend carries the winning index.
// Indices ride in float lanes (exact below 2^24; see volk.h).
void volk_32f_index_max_32u(uint32_t *target, const float *src0, uint32_t num_points)
{
    const v8 lanes0 = {0, 1, 2, 3, 4, 5, 6, 7};

    float m = 0.0f;
    uint32_t mi = 0;
    v8 maxv, idxv, off = lanes0;
    unsigned int i = 0;

    if (num_points >= 8)
    {
        V8_LOAD(maxv, src0);
        idxv = lanes0;
        i = 8;
        off = lanes0 + 8.0f;
        for (; i + 8 <= num_points; i += 8)
        {
            v8 x;
            V8_LOAD(x, src0 + i);
            iv8 cm = x > maxv; // strict: earlier equal maxima win
            idxv = volk_blend8(cm, off, idxv);
            maxv = volk_blend8(cm, x, maxv);
            off += 8.0f;
        }
        // lane reduction, ascending: strict > keeps the first maximum
        m = maxv[0];
        mi = (uint32_t)idxv[0];
        for (int l = 1; l < 8; l++)
            if (maxv[l] > m)
            {
                m = maxv[l];
                mi = (uint32_t)idxv[l];
            }
    }
    else if (num_points > 0)
    {
        m = src0[0];
    }

    for (; i < num_points; i++)
        if (src0[i] > m)
        {
            m = src0[i];
            mi = i;
        }

    *target = mi;
}
