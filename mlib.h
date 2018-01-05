#ifndef __MLIB_H__
#define __MLIB_H__

#ifdef __cplusplus
extern "C" {
#endif

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

#define compiler_barrier() asm volatile("" ::: "memory")

static inline void
tsc_sleep_till(uint64_t when)
{
    while (rdtsc() < when)
        compiler_barrier();
}

/* Prepend this to a struct field to make it aligned. */
#define CACHELINE_SIZE 64
#define CACHELINE_ALIGNED __attribute__((aligned(CACHELINE_SIZE)))

void runon(const char *prefix, int cpuid);

#ifdef __cplusplus
}
#endif

#endif /* __MLIB_H__ */
