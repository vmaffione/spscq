#ifndef __TSC_H__
#define __TSC_H__

#include <stdint.h>

void tsc_init(void);
uint64_t ns2tsc(uint64_t ns);
uint64_t tsc2ns(uint64_t tsc);

static inline uint64_t
rdtsc(void)
{
    uint32_t hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

#define barrier() asm volatile("" ::: "memory")

static inline void
tsc_sleep_till(uint64_t when)
{
    while (rdtsc() < when)
        barrier();
}

#endif /* __TSC_H__ */