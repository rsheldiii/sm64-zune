/* Test of the VFP-friendly libm replacements (compat/zune_math.h, platform/zune_libm.c)
 * against this machine's libm. zune_sqrtf is ARM assembly on the Zune and is not tested here.
 * tests/run.sh builds and runs it (./sm64zune test). */
#define ZUNE_MATH_NO_REDIRECT
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../platform/zune_libm.c"

float sinf(float);
float cosf(float);
float floorf(float);
float ceilf(float);
float fabsf(float);
float nextafterf(float, float);

static double worstSin, worstCos;

/* Error in units of the last place of the exact result (at least the ulp of 2^-24's binade,
 * so results near zero are judged absolutely). */
static double UlpError(float got, double exact)
{
    float reference = (float)fabs(exact);
    double ulp = (double)nextafterf(reference, 4.0f) - (double)reference;
    if (ulp < 1e-9) ulp = 1e-9;
    return fabs((double)got - exact) / ulp;
}

static void CheckTrig(float x)
{
    double s = UlpError(zune_sinf(x), sin((double)x)), c = UlpError(zune_cosf(x), cos((double)x));
    if (s > worstSin) worstSin = s;
    if (c > worstCos) worstCos = c;
    if (s > 1.0 || c > 1.0) {
        fprintf(stderr, "x = %.9g: sin %.9g (libm %.9g), cos %.9g (libm %.9g)\n", x, zune_sinf(x), sinf(x), zune_cosf(x), cosf(x));
        assert(0);
    }
}

static void CheckRounding(float x)
{
    if (zune_floorf(x) != floorf(x) || zune_ceilf(x) != ceilf(x) || zune_fabsf(x) != fabsf(x)) {
        fprintf(stderr, "x = %.9g: floor %.9g (libm %.9g), ceil %.9g (libm %.9g), fabs %.9g (libm %.9g)\n", x,
               zune_floorf(x), floorf(x), zune_ceilf(x), ceilf(x), zune_fabsf(x), fabsf(x));
        assert(0);
    }
}

int main(void)
{
    static const float edges[] = {0.0f, 0.5f, 1.0f, 1.5f, 2.5f, 319.999f, 320.0f, 8388607.5f, 8388608.0f, 8388609.0f,
                                  16777216.0f, 2147483648.0f, 4294967296.0f, 1e20f, 1e-20f, 1e-45f, 3.4e38f};
    unsigned i, seed = 12345;
    int n;
    /* The angles sm64 uses: a dense sweep over several turns, then sparser out to the limit. */
    for (n = -4000000; n <= 4000000; ++n) CheckTrig((float)n * (1.0f / 131072.0f));
    for (n = -100000; n <= 100000; ++n) CheckTrig((float)n * 0.999f);
    for (n = 0; n < 2000000; ++n) {
        seed = seed * 1664525u + 1013904223u;
        CheckTrig(((float)(seed >> 8) / 8388608.0f - 1.0f) * 99999.0f);
        CheckRounding(((float)(seed >> 8) / 8388608.0f - 1.0f) * 70000.0f);
        CheckRounding(((float)(seed >> 8) / 8388608.0f - 1.0f) * 9000000.0f);
    }
    /* Multiples of pi/2, where the reduction's remainder is smallest and the quadrant flips. */
    for (n = -60000; n <= 60000; ++n) {
        float x = (float)(n * 1.57079632679489661923);
        CheckTrig(x);
        CheckTrig(nextafterf(x, 1e9f));
        CheckTrig(nextafterf(x, -1e9f));
    }
    /* Outside the fast range the software path answers, as before. */
    assert(zune_sinf(1e9f) == (float)sin((double)1e9f) && zune_cosf(-1e9f) == (float)cos((double)-1e9f));
    assert(zune_sinf(100000.0f) == (float)sin(100000.0));
    for (n = -200000; n <= 200000; ++n) CheckRounding((float)n * 0.25f);
    for (i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        CheckRounding(edges[i]);
        CheckRounding(-edges[i]);
        CheckRounding(nextafterf(edges[i], 1e30f));
        CheckRounding(nextafterf(edges[i], -1e30f));
    }
    assert(zune_fabsf(-3.5f) == 3.5f && zune_floorf(-0.25f) == -1.0f && zune_ceilf(-0.25f) == 0.0f);
    assert(zune_floorf(2.75f) == 2.0f && zune_ceilf(2.25f) == 3.0f);
    printf("sinf worst %.3f ulp, cosf worst %.3f ulp\n", worstSin, worstCos);
    printf("libm tests passed\n");
    return 0;
}
