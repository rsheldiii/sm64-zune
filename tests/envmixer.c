/* Bit-exactness check for patches/0009-zune-envmixer-settled-volume.patch.
 * Built twice, with -DMIXER_C pointing at sm64ex's original mixer.c and at the patched copy
 * (scalar path: no SSE4.1/NEON on the build host flags); both must print the same hash over
 * random envelope-mixer calls, including continued (non-A_INIT) calls on saved state. */
#include MIXER_C
#include <stdio.h>

static uint32_t seed = 12345;
static uint32_t next_random(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static int16_t random_s16(void) { return (int16_t)(next_random() & 0xffff); }
static uint64_t hash = 1469598103934665603ull;
static void mix_bytes(const void *data, size_t size)
{
    const uint8_t *p = data;
    size_t i;
    for (i = 0; i < size; i++) hash = (hash ^ p[i]) * 1099511628211ull;
}

static int32_t random_rate(void)
{
    switch (next_random() % 5) {
    case 0: return 0x10000;                                   /* hold */
    case 1: return 0x10000 + (int32_t)(next_random() % 0x800);  /* slow rise */
    case 2: return 0x10000 - (int32_t)(next_random() % 0x800);  /* slow fall */
    case 3: return (int32_t)(next_random() % 0x30000);           /* anything */
    default: return 0x10000 + (int32_t)(next_random() % 0x8000) - 0x4000;
    }
}

int main(void)
{
    ENVMIX_STATE state;
    int t, call;
    for (t = 0; t < 20000; t++) {
        int16_t v0 = (int16_t)(next_random() % 0x8000), v1 = (int16_t)(next_random() % 0x8000);
        int16_t t0 = next_random() % 4 ? (int16_t)(next_random() % 0x8000) : v0;
        int16_t t1 = next_random() % 4 ? (int16_t)(next_random() % 0x8000) : v1;
        uint8_t aux = next_random() % 2 ? A_AUX : 0;
        int i;
        rspa.vol[0] = v0;
        rspa.vol[1] = v1;
        rspa.target[0] = t0;
        rspa.target[1] = t1;
        rspa.rate[0] = random_rate();
        rspa.rate[1] = next_random() % 3 ? rspa.rate[0] : random_rate();
        rspa.vol_dry = (int16_t)(next_random() % 0x8000);
        rspa.vol_wet = (int16_t)(next_random() % 0x8000);
        for (call = 0; call < 3; call++) {
            for (i = 0; i < 2512 / 2; i++) rspa.buf.as_s16[i] = random_s16();
            rspa.in = 0;
            rspa.out = 0x200;
            rspa.dry_right = 0x400;
            rspa.wet_left = 0x600;
            rspa.wet_right = 0x800;
            rspa.nbytes = (uint16_t)((1 + next_random() % 26) * 16);
            aEnvMixerImpl((uint8_t)((call == 0 ? A_INIT : 0) | aux), state);
            mix_bytes(rspa.buf.as_u8, sizeof(rspa.buf.as_u8));
            mix_bytes(state, sizeof(state));
        }
    }
    printf("%016llx\n", (unsigned long long)hash);
    return 0;
}
