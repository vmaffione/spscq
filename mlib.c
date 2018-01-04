#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>

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

void
runon(const char *name, int i)
{
    static int NUM_CPUS = 0;
    cpu_set_t cpumask;

    if (NUM_CPUS == 0) {
        NUM_CPUS = sysconf(_SC_NPROCESSORS_ONLN);
        printf("system has %d cores\n", NUM_CPUS);
    }
    CPU_ZERO(&cpumask);
    if (i >= 0) {
        CPU_SET(i, &cpumask);
    } else {
        /* -1 means it can run on any CPU */
        int j;

        i = -1;
        for (j = 0; j < NUM_CPUS; j++) {
            CPU_SET(j, &cpumask);
        }
    }

    if ((errno = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
                                        &cpumask)) != 0) {
        printf("Unable to set affinity for %s on %d : %s\n", name, i,
               strerror(errno));
    }

    if (i >= 0) {
        printf("thread %s on core %d\n", name, i);
    } else {
        printf("thread %s on any core in 0..%d\n", name, NUM_CPUS - 1);
    }
}
