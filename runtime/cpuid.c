/* The CPU the program is told it runs on: a Pentium II class processor without MMX, so
   SurRender keeps to its plain x87 paths (sr.dll's MMX colour routines are not translated) and
   picks the generic vector processor. */
#include <time.h>
#include "runtime.h"

void cpu_cpuid(CPU *c)
{
    uint32_t leaf = c->eax;
    switch (leaf) {
    case 0:
        c->eax = 1;
        c->ebx = 0x756e6547u;           /* "GenuineIntel" */
        c->edx = 0x49656e69u;
        c->ecx = 0x6c65746eu;
        break;
    case 1:
        c->eax = CPU_ID_VERSION;         /* family 6, model 5: Pentium II */
        c->ebx = 0;
        c->ecx = 0;
        c->edx = CPU_ID_FEATURES;        /* FPU, TSC, CX8, CMOV; no MMX */
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
        break;
    }
    LOG(LOG_API, "cpuid(%x)", leaf);
}

/* A 400 MHz time stamp counter from the host's clock (with --test, the virtual clock). */
uint64_t cpu_rdtsc(void)
{
    return (uint64_t)(now_ms() * (CPU_TSC_HZ / 1000.0));
}
