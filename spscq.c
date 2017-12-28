#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

#include "mlib.h"

#undef QDEBUG

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

static int
is_power_of_two(int x)
{
    return !x || !(x & (x - 1));
}

struct mbuf {
    unsigned int len;
    unsigned int __padding[7];
#define MBUF_LEN_MAX 1500
    char buf[MBUF_LEN_MAX];
};

#define mbuf_get(pool_, pool_idx_, pool_mask_)                                 \
    ({                                                                         \
        struct mbuf *m = &pool[pool_idx_ & pool_mask_];                        \
        m->len         = pool_idx_++;                                          \
        m;                                                                     \
    })

#define mbuf_put(m_, sum_) sum_ += m_->len

typedef void *(*pc_function_t)(void *);
struct msq;

struct global {
    /* Test length as a number of packets. */
    long int num_packets;

    /* Length of the SPSC queue. */
    unsigned int qlen;

    /* Max consumer batch. */
    unsigned int batch;

    /* Affinity for producer and consumer. */
    int p_core, c_core;

    const char *test_type;

    /* Timestamp to compute experiment statistics. */
    struct timespec begin, end;

    /* The queue. */
    struct msq *mq;

    /* A pool of preallocated mbufs. */
    struct mbuf *pool;
};

/*
 * Multi-section queue, based on the Lamport classic queue.
 * All indices are free running.
 */
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
    unsigned int batch;

    /* The queue. */
    CACHELINE_ALIGNED
    struct mbuf *q[0];
};

static struct msq *
msq_create(int qlen, int batch)
{
    struct msq *mq = szalloc(sizeof(*mq) + qlen * sizeof(mq->q[0]));

    if (qlen < 2 || !is_power_of_two(qlen)) {
        printf("Error: queue length %d is not a power of two\n", qlen);
        exit(EXIT_FAILURE);
    }

    mq->qlen  = qlen;
    mq->qmask = qlen - 1;
    mq->batch = batch;

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
    memset(mq, 0, sizeof(*mq));
    free(mq);
}

static void *
msq_legacy_producer(void *opaque)
{
    struct global *g       = (struct global *)opaque;
    long int left          = g->num_packets;
    unsigned int pool_mask = g->mq->qmask;
    struct mbuf *pool      = g->pool;
    struct msq *mq         = g->mq;
    unsigned int pool_idx  = 0;

    runon("P", g->p_core);

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
        struct mbuf *m = mbuf_get(pool, pool_idx, pool_mask);
#ifdef QDEBUG
        msq_dump("P", mq);
#endif
        if (msq_write(mq, m) == 0) {
            --left;
        } else {
            pool_idx--;
        }
    }
    msq_dump("P", mq);

    pthread_exit(NULL);
    return NULL;
}

static void *
msq_legacy_consumer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    long int left    = g->num_packets;
    struct msq *mq   = g->mq;
    unsigned int sum = 0;
    struct mbuf *m;

    runon("C", g->c_core);

    while (left > 0) {
#ifdef QDEBUG
        msq_dump("C", mq);
#endif
        m = msq_read(mq);
        if (m) {
            --left;
            mbuf_put(m, sum);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    msq_dump("C", mq);
    printf("[C] sum = %x\n", sum);

    pthread_exit(NULL);
    return NULL;
}

static void *
msq_producer(void *opaque)
{
    struct global *g       = (struct global *)opaque;
    long int left          = g->num_packets;
    unsigned int pool_mask = g->mq->qmask;
    unsigned int batch     = g->batch;
    struct mbuf *pool      = g->pool;
    struct msq *mq         = g->mq;
    unsigned int pool_idx  = 0;

    runon("P", g->p_core);

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
        unsigned int avail = msq_wspace(mq);

#ifdef QDEBUG
        msq_dump("P", mq);
#endif
        if (avail) {
            if (avail > batch) {
                avail = batch;
            }
#if 0
            /* Enable this to get a consistent 'sum' in the consumer. */
            if (avail > left) {
                avail = left;
            }
#endif
            left -= avail;
            for (; avail > 0; avail--) {
                struct mbuf *m = mbuf_get(pool, pool_idx, pool_mask);
                msq_write_local(mq, m);
            }
            msq_write_publish(mq);
        }
    }
    msq_dump("P", mq);

    pthread_exit(NULL);
    return NULL;
}

static void *
msq_consumer(void *opaque)
{
    struct global *g   = (struct global *)opaque;
    long int left      = g->num_packets;
    unsigned int batch = g->batch;
    struct msq *mq     = g->mq;
    unsigned int sum   = 0;
    struct mbuf *m;

    runon("C", g->c_core);

    while (left > 0) {
        unsigned int avail = msq_rspace(mq);

#ifdef QDEBUG
        msq_dump("C", mq);
#endif
        if (avail) {
            if (avail > batch) {
                avail = batch;
            }
            left -= avail;
            for (; avail > 0; avail--) {
                m = msq_read_local(mq);
                mbuf_put(m, sum);
            }
            msq_read_publish(mq);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    msq_dump("C", mq);
    printf("[C] sum = %x\n", sum);

    pthread_exit(NULL);
    return NULL;
}

static int
run_test(struct global *g)
{
    pc_function_t prod_func = NULL;
    pc_function_t cons_func = NULL;
    unsigned long int ndiff;
    pthread_t pth, cth;
    double mpps;

    if (!strcmp(g->test_type, "msql")) {
        prod_func = msq_legacy_producer;
        cons_func = msq_legacy_consumer;
    } else if (!strcmp(g->test_type, "msq")) {
        prod_func = msq_producer;
        cons_func = msq_consumer;
    } else {
        printf("Error: unknown test type '%s'\n", g->test_type);
        exit(EXIT_FAILURE);
    }

    g->mq   = msq_create(g->qlen, g->batch);
    g->pool = malloc(g->qlen * sizeof(g->pool[0]));

    if (pthread_create(&pth, NULL, prod_func, g)) {
        perror("pthread_create(producer)");
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&cth, NULL, cons_func, g)) {
        perror("pthread_create(consumer)");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(pth, NULL)) {
        perror("pthread_join(producer)");
        exit(EXIT_FAILURE);
    }

    if (pthread_join(cth, NULL)) {
        perror("pthread_join(consumer)");
        exit(EXIT_FAILURE);
    }

    ndiff = (g->end.tv_sec - g->begin.tv_sec) * 1000000000U +
            (g->end.tv_nsec - g->begin.tv_nsec);
    mpps = g->num_packets * 1000.0 / ndiff;
    printf("Throughput %3.3f Mpps\n", mpps);

    free(g->pool);
    msq_free(g->mq);

    return 0;
}

static void
usage(const char *progname)
{
    printf("%s [-h]\n"
           "    [-n NUM_PACKETS (in millions)]\n"
           "    [-b MAX_BATCH]\n"
           "    [-l QUEUE_LENGTH]\n"
           "    [-L NUM_MBUFS]\n"
           "    [-c PRODUCER_CORE_ID]\n"
           "    [-c CONSUMER_CORE_ID]\n"
           "    [-t TEST_TYPE (msql,msq,ff)]\n"
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
    g->batch       = 32;
    g->p_core      = -1;
    g->c_core      = -1;
    g->test_type   = "msql";

    while ((opt = getopt(argc, argv, "hn:b:l:c:t:")) != -1) {
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
            g->batch = atoi(optarg);
            if (g->batch < 1) {
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

        case 't':
            g->test_type = optarg;
            break;
        }
    }

    tsc_init();
    run_test(g);

    return 0;
}
