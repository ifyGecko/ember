/*
 * test_f80.c - The x87 80-bit format in software (isa/x86/x86_f80.c).
 *
 * On an x86-64 host every operation is checked bit for bit against the host's x87
 * (long double arithmetic, and the instructions themselves where C has no equivalent),
 * over special values and a random stream of operands of every class. On every host,
 * a digest of the results over a fixed stream must equal the one recorded from an
 * x86-64 host: the software gives the same answers everywhere.
 *
 *   test_f80 [N]   N random operand pairs (default 200000)
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../isa/x86/x86.h"

static unsigned checks, failures;

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; if (failures <= 40) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static x86_f80 mk(unsigned se, uint64_t m) { return (x86_f80){ m, (uint16_t)se }; }
static bool same(x86_f80 a, x86_f80 b) { return a.m == b.m && a.se == b.se; }

/* An operand of a random class, biased toward the edges. */
static x86_f80 rand_f80(void)
{
    const uint64_t r = next(), m = next();
    const unsigned s = (r >> 60 & 1) << 15;
    switch (r & 15) {
    case 0:  return mk(s, 0);                                        /* zero */
    case 1:  return mk(s | 0x7FFF, 1ull << 63);                      /* infinity */
    case 2:  return mk(s | 0x7FFF, 1ull << 63 | 1ull << 62 | (m >> 2)); /* QNaN */
    case 3:  return mk(s | 0x7FFF, 1ull << 63 | ((m >> 2) | 1));     /* SNaN */
    case 4:  return mk(s, m >> (r >> 8 & 63));                       /* denormal */
    case 5:  return mk(s | (unsigned)(1 + (r >> 8 & 127)), m | 1ull << 63);        /* tiny */
    case 6:  return mk(s | (unsigned)(0x7FFE - (r >> 8 & 127)), m | 1ull << 63);   /* huge */
    case 7:  return mk(s | (unsigned)(r >> 8 & 0x7FFF), m & ~(1ull << 63));        /* unsupported */
    case 8:  return mk(s | 0x3FFF, (m | 1ull << 63) & ~0xFFFFull);   /* short significands */
    case 9:  return mk(s | (unsigned)(0x3FFF + (r >> 8 & 63)), 1ull << 63 | (m & 0xFF00000000000000ull));
    default: return mk(s | (unsigned)(0x3FFF - 40 + (r >> 8 & 127)), m | 1ull << 63);
    }
}

/* The digest of the software's results over the fixed stream. */
static uint64_t digest = 0xCBF29CE484222325ull;
static void mix(uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) {
        digest ^= (uint8_t)(v >> (8 * i));
        digest *= 0x100000001B3ull;
    }
}
static void mixf(x86_f80 v) { mix(v.m); mix(v.se); }

#if defined(__x86_64__) && defined(__GNUC__) && LDBL_MANT_DIG == 64
#define HOST_X87 1
static long double ld(x86_f80 v) { long double r = 0; memcpy(&r, &v.m, 8); memcpy((char *)&r + 8, &v.se, 2); return r; }
static x86_f80 fx(long double v) { x86_f80 r; memcpy(&r.m, &v, 8); memcpy(&r.se, (char *)&v + 8, 2); return r; }

static void set_cw(unsigned short cw) { __asm__ volatile ("fldcw %0" : : "m"(cw)); }
#define CW(rc) ((unsigned short)(0x37F | (rc) << 10))

static long double hw_frndint(long double a, unsigned rc)
{
    long double r;
    set_cw(CW(rc));
    __asm__ ("frndint" : "=t"(r) : "0"(a));
    set_cw(CW(0));
    return r;
}
static long long hw_fistp(long double a, unsigned rc)
{
    long long r;
    set_cw(CW(rc));
    __asm__ ("fistpll %0" : "=m"(r) : "t"(a) : "st");
    set_cw(CW(0));
    return r;
}
static long double hw_fscale(long double a, long double b)
{
    long double r;
    __asm__ ("fscale" : "=t"(r) : "0"(a), "u"(b));
    return r;
}
/* one FPREM/FPREM1 step; *sw = the status word after it */
static long double hw_fprem(long double a, long double b, bool ieee, unsigned short *sw)
{
    long double r;
    if (ieee)
        __asm__ ("fprem1; fnstsw %%ax" : "=t"(r), "=a"(*sw) : "0"(a), "u"(b));
    else
        __asm__ ("fprem; fnstsw %%ax" : "=t"(r), "=a"(*sw) : "0"(a), "u"(b));
    return r;
}
#define HW_CONST(insn) ({ long double r_; __asm__ (insn : "=t"(r_)); r_; })

static const char *hex(x86_f80 v)
{
    static char buf[4][32];
    static unsigned k;
    char *b = buf[k++ & 3];
    snprintf(b, 32, "%04x:%016llx", v.se, (unsigned long long)v.m);
    return b;
}

static void vs_host(x86_f80 a, x86_f80 b)
{
    const volatile long double x = ld(a), y = ld(b);
    x86_f80 h, s;
    h = fx(x + y); s = x86_f80_add(a, b);
    CHECK(same(h, s), "add %s %s: host %s soft %s", hex(a), hex(b), hex(h), hex(s));
    h = fx(x - y); s = x86_f80_sub(a, b);
    CHECK(same(h, s), "sub %s %s: host %s soft %s", hex(a), hex(b), hex(h), hex(s));
    h = fx(x * y); s = x86_f80_mul(a, b);
    CHECK(same(h, s), "mul %s %s: host %s soft %s", hex(a), hex(b), hex(h), hex(s));
    h = fx(x / y); s = x86_f80_div(a, b);
    CHECK(same(h, s), "div %s %s: host %s soft %s", hex(a), hex(b), hex(h), hex(s));
    long double sq;
    __asm__ ("fsqrt" : "=t"(sq) : "0"(x));
    h = fx(sq); s = x86_f80_sqrt(a);
    CHECK(same(h, s), "sqrt %s: host %s soft %s", hex(a), hex(h), hex(s));
    const int c = isunordered(x, y) ? 2 : x < y ? -1 : x == y ? 0 : 1;
    CHECK(x86_f80_cmp(a, b) == c, "cmp %s %s: host %d soft %d", hex(a), hex(b), c, x86_f80_cmp(a, b));
    if (x86_f80_class(a) != X86_F80_UNSUPPORTED) {   /* the stores need a supported value */
        const volatile float f = (float)x;
        const volatile double d = (double)x;
        uint32_t fb; uint64_t db;
        memcpy(&fb, (const void *)&f, 4);
        memcpy(&db, (const void *)&d, 8);
        CHECK(fb == x86_f80_to_f32(a), "to f32 %s: host %08x soft %08x", hex(a), fb, x86_f80_to_f32(a));
        CHECK(db == x86_f80_to_f64(a), "to f64 %s: host %016llx soft %016llx", hex(a),
              (unsigned long long)db, (unsigned long long)x86_f80_to_f64(a));
        h = fx((long double)f); s = x86_f80_from_f32(fb);
        CHECK(same(h, s), "from f32 %08x: host %s soft %s", fb, hex(h), hex(s));
        h = fx((long double)d); s = x86_f80_from_f64(db);
        CHECK(same(h, s), "from f64 %016llx: host %s soft %s", (unsigned long long)db, hex(h), hex(s));
    }
    for (unsigned rc = 0; rc < 4; rc++) {
        bool inexact;
        h = fx(hw_frndint(x, rc)); s = x86_f80_round(a, rc, &inexact);
        CHECK(same(h, s), "frndint rc%u %s: host %s soft %s", rc, hex(a), hex(h), hex(s));
        int64_t iv;
        const long long hv = hw_fistp(x, rc);
        const int64_t sv = x86_f80_to_int(a, rc, 64, &iv, &inexact) ? iv : INT64_MIN;
        CHECK(hv == sv, "fistp rc%u %s: host %lld soft %lld", rc, hex(a), hv, (long long)sv);
    }
    h = fx(hw_fscale(x, y)); s = x86_f80_scale(a, b);
    CHECK(same(h, s), "fscale %s %s: host %s soft %s", hex(a), hex(b), hex(h), hex(s));
    for (int ieee = 0; ieee < 2; ieee++) {
        unsigned short sw;
        const long double r = hw_fprem(x, y, ieee, &sw);
        if (sw & 0x400)
            continue;                    /* a partial step (exponents 64 or more apart) */
        unsigned q;
        s = x86_f80_rem(a, b, ieee, &q);
        const unsigned hq = (sw >> 9 & 1) | (sw >> 13 & 2) | (sw >> 6 & 4);   /* C1 C3 C0 */
        const bool nan = x86_f80_class(s) == X86_F80_NAN;
        CHECK(same(fx(r), s) && (nan || q == hq), "fprem%s %s %s: host %s q%u soft %s q%u",
              ieee ? "1" : "", hex(a), hex(b), hex(fx(r)), hq, hex(s), q);
    }
}
#endif

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;

    /* fixed vectors, any host */
    const x86_f80 one = mk(0x3FFF, 1ull << 63), two = mk(0x4000, 1ull << 63), three = mk(0x4000, 3ull << 62);
    CHECK(same(x86_f80_add(one, one), two), "1 + 1");
    CHECK(same(x86_f80_div(one, three), mk(0x3FFD, 0xAAAAAAAAAAAAAAABull)), "1 / 3");
    CHECK(same(x86_f80_sqrt(two), mk(0x3FFF, 0xB504F333F9DE6484ull)), "sqrt 2");
    CHECK(same(x86_f80_sub(one, one), mk(0, 0)), "1 - 1 = +0");
    CHECK(same(x86_f80_div(mk(0, 0), mk(0, 0)), X86_F80_INDEFINITE), "0 / 0");
    CHECK(same(x86_f80_mul(mk(0, 1), mk(0x3FFE, 1ull << 63)), mk(0, 0)), "min denormal / 2 (to even)");
    CHECK(same(x86_f80_from_f64(0x3FF0000000000000ull), one), "from f64 1.0");
    CHECK(x86_f80_to_f32(three) == 0x40400000u, "to f32 3.0");
    {
        int64_t v; bool inexact;
        CHECK(x86_f80_to_int(mk(0x3FFF, 3ull << 62), 0, 64, &v, &inexact) && v == 2 && inexact, "1.5 to even");
        CHECK(!x86_f80_to_int(mk(0x403E, 1ull << 63), 0, 64, &v, &inexact), "2^63 is out of range");
        CHECK(x86_f80_to_int(mk(0xC03E, 1ull << 63), 0, 64, &v, &inexact) && v == INT64_MIN, "-2^63");
        unsigned q;
        CHECK(same(x86_f80_rem(mk(0x4001, 7ull << 61), two, true, &q), x86_f80_neg(one)) && q == 4,
              "fprem1 7 2");
    }
    {   /* the host's long double round trip, exact for values of 53 bits */
        const x86_f80 v = mk(0x3FFF, 0xC90FDAA22168C000ull);
        CHECK(same(x86_f80_from_ld(x86_f80_to_ld(v)), v), "long double round trip");
    }
#ifdef HOST_X87
    {
        const x86_f80 consts[5] = { fx(HW_CONST("fldl2t")), fx(HW_CONST("fldl2e")), fx(HW_CONST("fldpi")),
                                    fx(HW_CONST("fldlg2")), fx(HW_CONST("fldln2")) };
        const uint64_t want[5] = { 0xD49A784BCD1B8AFEull, 0xB8AA3B295C17F0BCull, 0xC90FDAA22168C235ull,
                                   0x9A209A84FBCFF799ull, 0xB17217F7D1CF79ACull };
        for (unsigned i = 0; i < 5; i++)
            CHECK(consts[i].m == want[i], "constant %u: host %016llx", i, (unsigned long long)consts[i].m);
    }
#endif

    /* the random stream: against the host's x87 if there is one, and into the digest */
    for (unsigned i = 0; i < n; i++) {
        const x86_f80 a = rand_f80(), b = rand_f80();
#ifdef HOST_X87
        vs_host(a, b);
#endif
        if (i < 100000) {
            bool inexact;
            unsigned q;
            int64_t iv = 0;
            mixf(x86_f80_add(a, b)); mixf(x86_f80_sub(a, b)); mixf(x86_f80_mul(a, b));
            mixf(x86_f80_div(a, b)); mixf(x86_f80_sqrt(a)); mix((uint64_t)x86_f80_cmp(a, b));
            mix(x86_f80_to_f32(a)); mix(x86_f80_to_f64(a));
            mixf(x86_f80_round(a, (unsigned)i & 3, &inexact));
            mix(x86_f80_to_int(a, (unsigned)i & 3, 64, &iv, &inexact) ? (uint64_t)iv : 1);
            mixf(x86_f80_scale(a, b)); mixf(x86_f80_rem(a, b, i & 1, &q)); mix(q);
        }
    }
    if (n >= 100000) {
        const uint64_t want = 0x7ACC62C1699CACBFull;
        CHECK(digest == want, "digest %016llx (recorded %016llx)", (unsigned long long)digest,
              (unsigned long long)want);
    }
    printf("test_f80: %u/%u checks passed\n", checks - failures, checks);
    return failures != 0;
}
