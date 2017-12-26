#include "tsc.h"
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

/* initialize to avoid a division by 0 */
uint64_t ticks_per_second = 1000000000; /* set by tsc_init() */

/*
 * An idle loop to compute how many clock ticks are there in a second.
 * We expect a constant TSC rate across all CPUs.
 */
void
tsc_init(void)
{
    struct timeval a, b;
    uint64_t ta_0, ta_1, tb_0, tb_1, dmax = ~0;
    uint64_t da, db, cy = 0;
    int i;
    for (i = 0; i < 3; i++) {
        ta_0 = rdtsc();
        gettimeofday(&a, NULL);
        ta_1 = rdtsc();
        usleep(20000);
        tb_0 = rdtsc();
        gettimeofday(&b, NULL);
        tb_1 = rdtsc();
        da   = ta_1 - ta_0;
        db   = tb_1 - tb_0;
        if (da + db < dmax) {
            cy   = (b.tv_sec - a.tv_sec) * 1000000 + b.tv_usec - a.tv_usec;
            cy   = (double)(tb_0 - ta_1) * 1000000 / (double)cy;
            dmax = da + db;
        }
    }
#if 0
    printf("dmax %lu, da %lu, db %lu, cy %lu\n", dmax, da, db, cy);
#endif
    ticks_per_second = cy;
}

uint64_t
ns2tsc(uint64_t ns)
{
    return ns * ticks_per_second / 1000000000UL;
}

uint64_t
tsc2ns(uint64_t tsc)
{
    return tsc * 1000000000UL / ticks_per_second;
}
