/* Hardware floating point for the libm calls the game makes on the Zune HD.
 *
 * VC9 compiles float arithmetic to VFP instructions (/QRfpe-), but every <math.h> function
 * is still a call into coredll, and coredll's math library is software floating point (it
 * also runs on ARMs without an FPU). One double sin() costs 13.7 us on the device. sm64 does
 * this constantly: sqrtf in vector maths and collision, fabsf for each fogged vertex, floorf
 * and ceilf for each HUD coordinate, 20-45 sinf/cosf calls for each shadow. In Bob-omb
 * Battlefield about 44% of the game-logic samples were inside coredll's software floating
 * point (sampling profile, October 5).
 *
 * The replacements stay on the VFP: sqrtf is the fsqrts instruction (platform/zune_vfp.asm),
 * sinf/cosf are polynomials (platform/zune_libm.c), and the rest are inline. Included from
 * sm64_vc9.h after <math.h>, so that header's own declarations are not renamed.
 * tests/libm.c checks them against the host libm (ZUNE_MATH_NO_REDIRECT). */
#ifndef ZUNE_MATH_H
#define ZUNE_MATH_H

#ifdef __cplusplus
extern "C" {
#endif
float zune_sqrtf(float x);
float zune_sinf(float x);
float zune_cosf(float x);
#ifdef __cplusplus
}
#endif

static __inline float zune_fabsf(float x) { return x < 0.0f ? -x : x; }

/* Floats of magnitude 2^23 and above have no fraction bits; below that the value fits an int. */
static __inline float zune_floorf(float x)
{
    float whole;
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    whole = (float)(int)x;   /* rounds toward zero */
    return whole > x ? whole - 1.0f : whole;
}

static __inline float zune_ceilf(float x)
{
    float whole;
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    whole = (float)(int)x;
    return whole < x ? whole + 1.0f : whole;
}

#ifndef ZUNE_MATH_NO_REDIRECT
#define sqrtf zune_sqrtf
#define sinf zune_sinf
#define cosf zune_cosf
#define fabsf zune_fabsf
#define floorf zune_floorf
#define ceilf zune_ceilf
#endif

#endif
