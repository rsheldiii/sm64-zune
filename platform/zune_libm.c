/* sinf and cosf for the Zune HD without coredll's software floating point (see
 * compat/zune_math.h). VFP double arithmetic is hardware, so the argument is reduced to
 * [-pi/4, pi/4] and the polynomials are evaluated in double: the first dropped Taylor terms
 * are r^11/11! < 1.8e-9 and r^12/12! < 1.2e-10, well under half a float ulp, so results are
 * within one ulp of a correctly rounded sinf/cosf (tests/libm.c).
 * atan2f comes from the game itself (src/engine/math_util.c). */
#include <math.h>
#include "zune_math.h"

#define PI_OVER_2 1.57079632679489661923
#define TWO_OVER_PI 0.63661977236758134308
/* Beyond this the single-constant reduction loses accuracy; sm64 passes angles of a few turns. */
#define REDUCIBLE 100000.0f

static double kernel_sin(double r)
{
    double z = r * r;
    return r + r * z * (-1.0 / 6 + z * (1.0 / 120 + z * (-1.0 / 5040 + z * (1.0 / 362880))));
}

static double kernel_cos(double r)
{
    double z = r * r;
    return 1.0 + z * (-1.0 / 2 + z * (1.0 / 24 + z * (-1.0 / 720 + z * (1.0 / 40320 + z * (-1.0 / 3628800)))));
}

/* x = quadrant * pi/2 + *remainder, with |*remainder| <= pi/4. */
static int reduce(float x, double *remainder)
{
    double turns = (double)x * TWO_OVER_PI;
    int quadrant = (int)(turns < 0 ? turns - 0.5 : turns + 0.5);
    *remainder = (double)x - quadrant * PI_OVER_2;
    return quadrant;
}

float zune_sinf(float x)
{
    double r;
    if (!(x > -REDUCIBLE && x < REDUCIBLE)) return (float)sin((double)x);   /* huge or NaN: software */
    switch (reduce(x, &r) & 3) {
    case 0: return (float)kernel_sin(r);
    case 1: return (float)kernel_cos(r);
    case 2: return (float)-kernel_sin(r);
    default: return (float)-kernel_cos(r);
    }
}

float zune_cosf(float x)
{
    double r;
    if (!(x > -REDUCIBLE && x < REDUCIBLE)) return (float)cos((double)x);
    switch (reduce(x, &r) & 3) {
    case 0: return (float)kernel_cos(r);
    case 1: return (float)-kernel_sin(r);
    case 2: return (float)-kernel_cos(r);
    default: return (float)kernel_sin(r);
    }
}
