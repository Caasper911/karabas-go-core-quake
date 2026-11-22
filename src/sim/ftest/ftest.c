/* Quake Ф0: RV32F check, ISS against the VexRiscv FpuPlugin core in Verilator (tb_soc).
   Every F instruction kind on a set of values (normal, zero, -0, inf, NaN, subnormal, big, negative),
   results printed as hex to the UART; MMIO_FRAME 0 first (iss --dump-at 0 makes the tb_soc start image),
   then the same lines in both sims = same semantics. Build: make -C src/sim/ftest; run: run.sh. */
#include <stdint.h>
#include "../../fw/mmio.h"

static void putc_(char c) { MMIO_UART = (unsigned char)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void hex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static uint32_t bits(float f) { union { float f; uint32_t u; } x = { .f = f }; return x.u; }
static float fl(uint32_t u) { union { float f; uint32_t u; } x = { .u = u }; return x.f; }

static const uint32_t V[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40490fdb, 0xc2c80000, 0x7f800000, 0xff800000,
    0x7fc00000, 0x00000001, 0x007fffff, 0x7f7fffff, 0x4f000000, 0xcf000001, 0x3effffff, 0x3f000000,
    0x3fc00000, 0x40200000, 0xc0200000, 0x4b7fffff, 0x358637bd, 0x501502f9,
};
#define NV (sizeof(V) / sizeof(V[0]))

static void line(const char *op, uint32_t a, uint32_t b, uint32_t r)
{
    puts_(op); putc_(' '); hex(a); putc_(' '); hex(b); putc_(' '); hex(r); putc_('\n');
}

int main(void)
{
    MMIO_FRAME = 0;
    for (unsigned i = 0; i < NV; i++) {
        float a = fl(V[i]);
        uint32_t r;
        { float f; __asm__ volatile ("fsqrt.s %0, %1" : "=f"(f) : "f"(a)); line("sqrt", V[i], 0, bits(f)); }
        __asm__ volatile ("fcvt.w.s %0, %1, rtz" : "=r"(r) : "f"(a)); line("cvtwz", V[i], 0, r);
        __asm__ volatile ("fcvt.w.s %0, %1, rne" : "=r"(r) : "f"(a)); line("cvtwn", V[i], 0, r);
        __asm__ volatile ("fcvt.w.s %0, %1, rdn" : "=r"(r) : "f"(a)); line("cvtwd", V[i], 0, r);
        __asm__ volatile ("fcvt.w.s %0, %1, rup" : "=r"(r) : "f"(a)); line("cvtwu", V[i], 0, r);
        __asm__ volatile ("fcvt.wu.s %0, %1, rtz" : "=r"(r) : "f"(a)); line("cvtuz", V[i], 0, r);
        __asm__ volatile ("fclass.s %0, %1" : "=r"(r) : "f"(a)); line("class", V[i], 0, r);
        { float f; __asm__ volatile ("fcvt.s.w %0, %1" : "=f"(f) : "r"(V[i])); line("cvtsw", V[i], 0, bits(f)); }
        { float f; __asm__ volatile ("fcvt.s.wu %0, %1" : "=f"(f) : "r"(V[i])); line("cvtsu", V[i], 0, bits(f)); }
        for (unsigned j = 0; j < NV; j++) {
            float b = fl(V[j]), f;
            line("add", V[i], V[j], bits(a + b));
            line("sub", V[i], V[j], bits(a - b));
            line("mul", V[i], V[j], bits(a * b));
            line("div", V[i], V[j], bits(a / b));
            __asm__ volatile ("fmadd.s %0, %1, %2, %3" : "=f"(f) : "f"(a), "f"(b), "f"(fl(0x3fc00000))); line("madd", V[i], V[j], bits(f));
            __asm__ volatile ("fnmsub.s %0, %1, %2, %3" : "=f"(f) : "f"(a), "f"(b), "f"(fl(0x40200000))); line("nmsub", V[i], V[j], bits(f));
            __asm__ volatile ("fmin.s %0, %1, %2" : "=f"(f) : "f"(a), "f"(b)); line("min", V[i], V[j], bits(f));
            __asm__ volatile ("fmax.s %0, %1, %2" : "=f"(f) : "f"(a), "f"(b)); line("max", V[i], V[j], bits(f));
            __asm__ volatile ("fsgnjx.s %0, %1, %2" : "=f"(f) : "f"(a), "f"(b)); line("sgnjx", V[i], V[j], bits(f));
            __asm__ volatile ("flt.s %0, %1, %2" : "=r"(r) : "f"(a), "f"(b)); line("lt", V[i], V[j], r);
            __asm__ volatile ("fle.s %0, %1, %2" : "=r"(r) : "f"(a), "f"(b)); line("le", V[i], V[j], r);
            __asm__ volatile ("feq.s %0, %1, %2" : "=r"(r) : "f"(a), "f"(b)); line("eq", V[i], V[j], r);
        }
    }
    puts_("done\n");
    MMIO_EXIT = 0;
    for (;;) {}
}
