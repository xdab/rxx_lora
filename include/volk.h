/*
 * Vendored VOLK kernels — the subset this receiver uses, one module, no
 * library dependency.
 *
 * Source: VOLK, the Vector-Optimized Library of Kernels
 *         https://github.com/gnuradio/volk (gnuradio/volk @
 *         v3.3.0-83-g453f289), licensed LGPL-3.0-or-later.
 *
 * The kernels keep their upstream names and prototypes so the origin of
 * every call stays obvious; each one cites its upstream file and variant.
 * Adaptations from upstream: the AVX2 intrinsics were ported to GCC vector
 * extensions (the house idiom, cf. resample.c / sfd.c) so the module needs
 * no dispatcher and no intrinsics headers; element-wise kernels keep the
 * upstream generic C verbatim.
 */

#ifndef VOLK_H
#define VOLK_H

#include <complex.h>
#include <stdint.h>

typedef float complex lv_32fc_t; // upstream's 32-bit complex type (C99 form)

/**
 * @brief c[i] = a[i] * b[i], complex element-wise.
 *        Upstream: volk_32fc_x2_multiply_32fc.h (u_avx2_fma variant).
 */
void volk_32fc_x2_multiply_32fc(
    lv_32fc_t *cVector,
    const lv_32fc_t *aVector,
    const lv_32fc_t *bVector,
    unsigned int num_points);

/**
 * @brief magnitudeVector[i] = |c[i]|^2.
 *        Upstream: volk_32fc_magnitude_squared_32f.h (u_avx variant).
 */
void volk_32fc_magnitude_squared_32f(
    float *magnitudeVector, const lv_32fc_t *complexVector, unsigned int num_points);

/**
 * @brief c[i] = conj(a[i]).
 *        Upstream: volk_32fc_conjugate_32fc.h (generic + AVX sign-mask form).
 */
void volk_32fc_conjugate_32fc(
    lv_32fc_t *cVector, const lv_32fc_t *aVector, unsigned int num_points);

/**
 * @brief result = sum input[i] * taps[i].
 *        Upstream: volk_32f_x2_dot_prod_32f.h (generic, vectorized here).
 */
void volk_32f_x2_dot_prod_32f(
    float *result, const float *input, const float *taps, unsigned int num_points);

/**
 * @brief result = sum inputBuffer[i].
 *        Upstream: volk_32f_accumulator_s32f.h (generic, vectorized here).
 */
void volk_32f_accumulator_s32f(float *result, const float *inputBuffer, unsigned int num_points);

/**
 * @brief cVector[i] = aVector[i] + bVector[i].
 *        Upstream: volk_32f_x2_add_32f.h (generic, verbatim).
 */
void volk_32f_x2_add_32f(
    float *cVector, const float *aVector, const float *bVector, unsigned int num_points);

/**
 * @brief cVector[i] = aVector[i] - bVector[i].
 *        Upstream: volk_32f_x2_subtract_32f.h (generic, verbatim).
 */
void volk_32f_x2_subtract_32f(
    float *cVector, const float *aVector, const float *bVector, unsigned int num_points);

/**
 * @brief target = index of the first maximum in src0 (first-max tie rule).
 *        Indices are tracked in float lanes, so num_points must stay below
 *        2^24 (this codebase peaks at 2^16).
 *        Upstream: volk_32f_index_max_32u.h (sse/avx2 blend form, ported).
 */
void volk_32f_index_max_32u(uint32_t *target, const float *src0, uint32_t num_points);

#endif // VOLK_H
