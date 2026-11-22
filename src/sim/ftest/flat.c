/* Quake Ф3г: latency of the VexRiscv FpuPlugin per operation kind, in a dependent chain (each op needs the
   previous result) and in an independent stream (4 chains interleaved). Clocks from MMIO_CYCLES; run on
   tb_soc (src/sim/ftest/flat.sh). Prints "<kind> dep <clocks/op> ind <clocks/op>". */
#include <stdint.h>
#include "../../fw/mmio.h"

static void putc_(char c) { MMIO_UART = (unsigned char)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void dec(uint32_t v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); puts_(b + i); }
static void fix(uint32_t x100) { dec(x100 / 100); putc_('.'); putc_('0' + x100 / 10 % 10); putc_('0' + x100 % 10); }

#define N 256
#define R8(x) x x x x x x x x
#define R64(x) R8(R8(x))
#define R256(x) R64(x) R64(x) R64(x) R64(x)

static void report(const char *k, uint32_t dep, uint32_t ind)
{
    puts_(k); puts_(" dep "); fix(dep * 100 / N); puts_(" ind "); fix(ind * 100 / N); putc_('\n');
}

/* twice: the first run warms the I$ (the code comes from the SDRAM), the second is measured */
#define MEASURE(var, body) do { for (int _w = 0; _w < 2; _w++) { uint32_t t0 = MMIO_CYCLES; body; var = MMIO_CYCLES - t0; } } while (0)

int main(void)
{
    MMIO_FRAME = 0;
    volatile float vin = 1.0001f;
    float a = vin, b = 0.9999f, c = 0.5f, d = 1.5f;
    float a1 = a, a2 = a, a3 = a;
    int ia = 3;
    static float mem[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    uint32_t dep, ind, base;

    /* the loop / read overhead: an empty block of nops */
    MEASURE(base, __asm__ volatile (R256("nop\n")));
    puts_("nop "); fix(base * 100 / N); putc_('\n');

#define TWO(name, ins)                                                                                   \
    MEASURE(dep, __asm__ volatile (R256(ins " %0, %0, %1\n") : "+f"(a) : "f"(b)));                       \
    MEASURE(ind, __asm__ volatile (R64(ins " %0, %0, %4\n" ins " %1, %1, %4\n" ins " %2, %2, %4\n" ins " %3, %3, %4\n") \
                                   : "+f"(a), "+f"(a1), "+f"(a2), "+f"(a3) : "f"(b)));                    \
    report(name, dep, ind);

    TWO("fadd", "fadd.s")
    TWO("fmul", "fmul.s")
    TWO("fdiv", "fdiv.s")
    TWO("fmin", "fmin.s")
    TWO("fsgnj", "fsgnj.s")

    MEASURE(dep, __asm__ volatile (R256("fmadd.s %0, %0, %1, %2\n") : "+f"(a) : "f"(b), "f"(c)));
    MEASURE(ind, __asm__ volatile (R64("fmadd.s %0, %0, %4, %5\nfmadd.s %1, %1, %4, %5\nfmadd.s %2, %2, %4, %5\nfmadd.s %3, %3, %4, %5\n")
                                   : "+f"(a), "+f"(a1), "+f"(a2), "+f"(a3) : "f"(b), "f"(c)));
    report("fmadd", dep, ind);

    a = d;
    MEASURE(dep, __asm__ volatile (R256("fsqrt.s %0, %0\n") : "+f"(a)));
    MEASURE(ind, __asm__ volatile (R64("fsqrt.s %0, %0\nfsqrt.s %1, %1\nfsqrt.s %2, %2\nfsqrt.s %3, %3\n") : "+f"(a), "+f"(a1), "+f"(a2), "+f"(a3)));
    report("fsqrt", dep, ind);

    /* float -> int -> float: the round trip through the integer registers */
    a = d;
    MEASURE(dep, __asm__ volatile (R256("fcvt.w.s %1, %0, rtz\nfcvt.s.w %0, %1\n") : "+f"(a), "+r"(ia)));
    report("fcvt w+s", dep, dep);
    /* compare -> branch-free use */
    MEASURE(dep, __asm__ volatile (R256("flt.s %1, %0, %2\nfcvt.s.w %0, %1\n") : "+f"(a), "+r"(ia) : "f"(b)));
    report("flt+cvt", dep, dep);
    /* load -> use */
    MEASURE(dep, __asm__ volatile (R256("flw %0, 0(%1)\nfadd.s %0, %0, %0\n") : "+f"(a) : "r"(mem)));
    report("flw+fadd", dep, dep);
    /* fmv.x.w round trip */
    MEASURE(dep, __asm__ volatile (R256("fmv.x.w %1, %0\nfmv.w.x %0, %1\n") : "+f"(a), "+r"(ia)));
    report("fmv x+w", dep, dep);

    puts_("done\n");
    MMIO_EXIT = 0;
    for (;;) {}
}
