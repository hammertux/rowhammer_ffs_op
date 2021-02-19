#ifndef ASM_H                                                                                                                                             
#define ASM_H

#include <inttypes.h>

__always_inline uint64_t rdtscp(void)
{
    uint64_t high, low;
    asm volatile("rdtscp" : "=a" (low), "=d" (high) :: "rcx");
    return ((high << 32) | low);
}

__always_inline void clflush(volatile uint8_t *addr)
{
    asm volatile("clflush (%0)" :: "r" (addr) : "memory");
}

__always_inline void lfence(void)
{
    asm volatile("lfence" ::: "memory");
}

__always_inline void mfence(void)
{
    asm volatile("mfence" ::: "memory");
}

__always_inline void ddr4_clflush(volatile uint8_t *addr)
{
    asm volatile("clflushopt (%0)" :: "r" (addr) : "memory");
}

#endif

