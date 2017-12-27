#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>

#include "tsc.h"

#undef QDEBUG

#define CACHELINE_ALIGNED __attribute__((aligned(64)))

/* Alloc zeroed memory, aborting on failure. */
static void *
szalloc(size_t size)
{
    void *p = malloc(size);
    if (!p) {
        exit(EXIT_FAILURE);
    }
    memset(p, 0, size);
    return p;
}

static void
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

struct mbuf {
    unsigned int len;
    unsigned int __padding[7];
#define MBUF_LEN_MAX 1500
    char buf[MBUF_LEN_MAX];
};

struct mbuf *
mbuf_alloc()
{
    return szalloc(sizeof(struct mbuf));
}

void
mbuf_free(struct mbuf *m)
{
    if (m) {
        free(m);
    }
}

/* Multi-section queue, based on the Lamport classic queue.
 * All indices are free running. */
struct msq {
    /* Producer private data. */
    CACHELINE_ALIGNED
    unsigned int write_priv;

    /* Producer write, consumer read. */
    CACHELINE_ALIGNED
    volatile unsigned int write;

    /* Consumer private data. */
    CACHELINE_ALIGNED
    unsigned int read_priv;

    /* Producer read, consumer write. */
    CACHELINE_ALIGNED
    volatile unsigned int read;

    /* Shared read only data. */
    CACHELINE_ALIGNED
    unsigned int qlen;
    unsigned int qmask;
    unsigned int rbatch;

    /* The queue. */
    CACHELINE_ALIGNED
    struct mbuf *q[0];
};

static struct msq *
msq_create(int qlen, int rbatch)
{
    struct msq *mq = szalloc(sizeof(*mq) + qlen * sizeof(mq->q[0]));

    if (qlen < 2 || ((qlen & (qlen - 1)) != 0)) {
        printf("Error: queue length %d is not a power of two\n", qlen);
        exit(EXIT_FAILURE);
    }

    mq->qlen   = qlen;
    mq->qmask  = qlen - 1;
    mq->rbatch = rbatch;

    return mq;
}

static inline unsigned int
msq_wspace(struct msq *mq)
{
    return (mq->read - 1 - mq->write_priv) & mq->qmask;
}

/* No boundary checks, to be called after msq_wspace(). */
static inline void
msq_write_local(struct msq *mq, struct mbuf *m)
{
    mq->q[mq->write_priv & mq->qmask] = m;
    mq->write_priv++;
}

static inline void
msq_write_publish(struct msq *mq)
{
    mq->write = mq->write_priv;
}

static inline int
msq_write(struct msq *mq, struct mbuf *m)
{
    if (msq_wspace(mq) == 0) {
        return -1; /* no space */
    }
    msq_write_local(mq, m);
    msq_write_publish(mq);

    return 0;
}

static inline unsigned int
msq_rspace(struct msq *mq)
{
    return mq->write - mq->read_priv;
}

/* No boundary checks, to be called after msq_rspace(). */
static inline struct mbuf *
msq_read_local(struct msq *mq)
{
    struct mbuf *m = mq->q[mq->read_priv & mq->qmask];
    mq->read_priv++;
    return m;
}

static inline void
msq_read_publish(struct msq *mq)
{
    mq->read = mq->read_priv;
}

static inline struct mbuf *
msq_read(struct msq *mq)
{
    struct mbuf *m;

    if (mq->read_priv == mq->write) {
        return NULL; /* no space */
    }
    m = msq_read_local(mq);
    msq_read_publish(mq);

    return m;
}

static void
msq_dump(const char *prefix, struct msq *mq)
{
    printf("[%s] r %u rspace %u w %u wspace %u\n", prefix, mq->read & mq->qmask,
           msq_rspace(mq), mq->write & mq->qmask, msq_wspace(mq));
}

static void
msq_free(struct msq *mq)
{
    struct mbuf *m;

    while ((m = msq_read(mq)) != NULL) {
        mbuf_free(m);
    }

    memset(mq, 0, sizeof(*mq));
    free(mq);
}

struct global {
    /* Test length as a number of packets. */
    long int num_packets;

    /* Queue length. */
    unsigned int qlen;

    /* Max consumer batch. */
    unsigned int rbatch;

    /* Affinity for producer and consumer. */
    int p_core, c_core;

    /* Timestamp to compute experiment statistics. */
    struct timespec begin, end;

    /* The queue. */
    struct msq *mq;
};

static void *
msq_producer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    long int left    = g->num_packets;
    struct mbuf *m   = mbuf_alloc();
    struct msq *mq   = g->mq;

    runon("P", g->p_core);

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
#ifdef QDEBUG
        msq_dump("P", mq);
#endif
        if (msq_write(mq, m) == 0) {
            --left;
        }
    }
    msq_dump("P", mq);

    pthread_exit(NULL);
    return NULL;
}

static void *
msq_consumer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    long int left    = g->num_packets;
    struct msq *mq   = g->mq;
    struct mbuf *m;

    runon("C", g->c_core);

    while (left > 0) {
#ifdef QDEBUG
        msq_dump("C", mq);
#endif
        m = msq_read(mq);
        if (m) {
            --left;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    msq_dump("C", mq);

    pthread_exit(NULL);
    return NULL;
}

static int
run_test(struct global *g)
{
    unsigned long int ndiff;
    pthread_t pth, cth;
    double mpps;

    g->mq = msq_create(g->qlen, g->rbatch);

    if (pthread_create(&pth, NULL, msq_producer, g)) {
        perror("pthread_create(msq_producer");
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&cth, NULL, msq_consumer, g)) {
        perror("pthread_create(msq_consumer)");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(pth, NULL)) {
        perror("pthread_join(msq_producer)");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(cth, NULL)) {
        perror("pthread_join(msq_consumer)");
        exit(EXIT_FAILURE);
    }

    ndiff = (g->end.tv_sec - g->begin.tv_sec) * 1000000000U +
            (g->end.tv_nsec - g->begin.tv_nsec);
    mpps = g->num_packets * 1000.0 / ndiff;
    printf("Throughput %3.3f Mpps\n", mpps);

    msq_free(g->mq);

    return 0;
}

static void
usage(const char *progname)
{
    printf("%s [-h]\n"
           "    [-n NUM_PACKETS (in millions)]\n"
           "    [-b MAX_CONSUMER_BATCH]\n"
           "    [-l QUEUE_LENGTH]\n"
           "    [-c PRODUCER_CORE_ID)]\n"
           "    [-c CONSUMER_CORE_ID)]\n"
           "\n",
           progname);
}

int
main(int argc, char **argv)
{
    struct global _g;
    struct global *g = &_g;
    int opt;

    memset(g, 0, sizeof(*g));
    g->num_packets = 10 * 1000000;
    g->qlen        = 128;
    g->rbatch      = 4;
    g->p_core      = -1;
    g->c_core      = -1;

    while ((opt = getopt(argc, argv, "hn:b:l:c:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            g->num_packets = atoi(optarg) * 1000000;
            if (g->num_packets < 0) {
                printf("    Invalid number of packets '%s'\n", optarg);
                return -1;
            }
            break;

        case 'b':
            g->rbatch = atoi(optarg);
            if (g->rbatch < 1) {
                printf("    Invalid receiver batch '%s'\n", optarg);
                return -1;
            }
            break;

        case 'l':
            g->qlen = atoi(optarg);
            if (g->qlen < 2) {
                printf("    Invalid queue length '%s'\n", optarg);
                return -1;
            }
            break;

        case 'c': {
            int val = atoi(optarg);
            if (g->p_core < 0) {
                g->p_core = val;
            } else if (g->c_core < 0) {
                g->c_core = val;
            }
            break;
        }
        }
    }

    tsc_init();
    run_test(g);

    return 0;
}
