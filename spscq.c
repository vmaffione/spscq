#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

#include "mlib.h"

#undef QDEBUG /* dump queue state at each operation */
#undef RATE  /* periodically print rate estimates */

#define HUNDREDMILLIONS (100LL * 1000000LL) /* 100 millions */
#define ONEBILLION (1000LL * 1000000LL)     /* 1 billion */

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

static unsigned int
roundup(unsigned int x, unsigned int y)
{
    return ((x + (y - 1)) / y) * y;
}

static unsigned long int
ilog2(unsigned long int x)
{
    unsigned long int probe = 0x00000001U;
    unsigned long int ret   = 0;
    unsigned long int c;

    assert(x != 0);

    for (c = 0; probe != 0; probe <<= 1, c++) {
        if (x & probe) {
            ret = c;
        }
    }

    return ret;
}

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

/* Ugly but useful macros for online rate estimation. */
#define RATE_HEADER(g_)                                                        \
    long long int thresh_ = g_->num_packets - HUNDREDMILLIONS;                 \
    struct timespec rate_last_;                                                \
    clock_gettime(CLOCK_MONOTONIC, &rate_last_);

#define RATE_BODY(left_)                                                       \
    if (unlikely(left_ < thresh_)) {                                           \
        unsigned long long int ndiff;                                          \
        struct timespec now;                                                   \
        double mpps;                                                           \
        clock_gettime(CLOCK_MONOTONIC, &now);                                  \
        ndiff = (now.tv_sec - rate_last_.tv_sec) * ONEBILLION +                \
                (now.tv_nsec - rate_last_.tv_nsec);                            \
        mpps = HUNDREDMILLIONS * 1000.0 / ndiff;                               \
        printf("%3.3f Mpps\n", mpps);                                          \
        thresh_ -= HUNDREDMILLIONS;                                            \
        rate_last_ = now;                                                      \
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
    long long int num_packets;

    /* Length of the SPSC queue. */
    unsigned int qlen;

    /* Max consumer batch. */
    unsigned int batch;

    /* Affinity for producer and consumer. */
    int p_core, c_core;

    const char *test_type;

    /* Timestamp to compute experiment statistics. */
    struct timespec begin, end;

    /* The lamport-like queue. */
    struct msq *mq;

    /* The ff-like queue. */
    struct iffq *fq;

    /* A pool of preallocated mbufs. */
    struct mbuf *pool;
};

/*
 * Multi-section queue, based on the Lamport classic queue.
 * All indices are free running.
 */
typedef struct mbuf *msq_entry_t; /* trick to use volatile */
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
    volatile msq_entry_t q[0];
};

static struct msq *
msq_create(int qlen, int batch)
{
    struct msq *mq = szalloc(sizeof(*mq) + qlen * sizeof(mq->q[0]));

    if (qlen < 2 || !is_power_of_two(qlen)) {
        printf("Error: queue length %d is not a power of two\n", qlen);
        return NULL;
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
    long long int left     = g->num_packets;
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
    struct global *g   = (struct global *)opaque;
    long long int left = g->num_packets;
    struct msq *mq     = g->mq;
    unsigned int sum   = 0;
    struct mbuf *m;
#ifdef RATE
    RATE_HEADER(g);
#endif

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
#ifdef RATE
        RATE_BODY(left);
#endif
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
    long long int left     = g->num_packets;
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
#ifndef RATE
            /* Enable this to get a consistent 'sum' in the consumer. */
            if (unlikely(avail > left)) {
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
    long long int left = g->num_packets;
    unsigned int batch = g->batch;
    struct msq *mq     = g->mq;
    unsigned int sum   = 0;
    struct mbuf *m;
#ifdef RATE
    RATE_HEADER(g);
#endif

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
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    msq_dump("C", mq);
    printf("[C] sum = %x\n", sum);

    pthread_exit(NULL);
    return NULL;
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/*
 * Improved FastForward queue, used by pspat.
 */
struct iffq {
    /* shared (constant) fields */
    unsigned long entry_mask;
    unsigned long seqbit_shift;
    unsigned long line_entries;
    unsigned long line_mask;

    /* producer fields */
    CACHELINE_ALIGNED
    unsigned long prod_write;
    unsigned long prod_check;

    /* consumer fields */
    CACHELINE_ALIGNED
    unsigned long cons_clear;
    unsigned long cons_read;

    /* the queue */
    CACHELINE_ALIGNED
    volatile uintptr_t q[0];
};

static inline size_t
iffq_size(unsigned long entries)
{
    return roundup(sizeof(struct iffq) + entries * sizeof(uintptr_t), 64);
}

/**
 * iffq_init - initialize a pre-allocated mailbox
 * @m: the mailbox to be initialized
 * @entries: the number of entries
 * @line_size: the line size in bytes
 *
 * Both entries and line_size must be a power of 2.
 * Returns 0 on success, -errno on failure.
 */
int
iffq_init(struct iffq *m, unsigned long entries, unsigned long line_size)
{
    unsigned long entries_per_line;

    if (!is_power_of_two(entries) || !is_power_of_two(line_size) ||
        entries <= 2 * line_size || line_size < sizeof(uintptr_t)) {
        printf("Error: invalid entries/linesize parameters\n");
        return -EINVAL;
    }

    entries_per_line = line_size / sizeof(uintptr_t);

    m->line_entries = entries_per_line;
    m->line_mask    = ~(entries_per_line - 1);
    m->entry_mask   = entries - 1;
    m->seqbit_shift = ilog2(entries);

    printf("iffq: line_entries %lu line_mask %lx entry_mask %lx seqbit_shift "
           "%lu\n",
           m->line_entries, m->line_mask, m->entry_mask, m->seqbit_shift);

    m->cons_clear = 0;
    m->cons_read  = m->line_entries;
    m->prod_write = m->line_entries;
    m->prod_check = 2 * m->line_entries;

    return 0;
}

/**
 * iffq_create - create a new mailbox
 * @entries: the number of entries
 * @line_size: the line size in bytes (not in entries)
 *
 * Both entries and line_size must be a power of 2.
 */
struct iffq *
iffq_create(unsigned long entries, unsigned long line_size)
{
    struct iffq *m;
    int err;

    m = szalloc(iffq_size(entries));
    if (m == NULL) {
        return NULL;
    }

    err = iffq_init(m, entries, line_size);
    if (err) {
        free(m);
        return NULL;
    }

    return m;
}

/**
 * iffq_free - delete a mailbox
 * @m: the mailbox to be deleted
 */
void
iffq_free(struct iffq *m)
{
    free(m);
}

void
iffq_dump(const char *prefix, struct iffq *fq)
{
    printf("[%s]: cc %lu, cr %lu, pw %lu, pc %lu\n", prefix, fq->cons_clear,
           fq->cons_read, fq->prod_write, fq->prod_check);
}

/**
 * iffq_insert - enqueue a new value
 * @m: the mailbox where to enqueue
 * @v: the value to be enqueued
 *
 * Returns 0 on success, -ENOBUFS on failure.
 */
static inline int
iffq_insert(struct iffq *fq, struct mbuf *m)
{
    volatile uintptr_t *h = &fq->q[fq->prod_write & fq->entry_mask];

    if (unlikely(fq->prod_write == fq->prod_check)) {
        /* Leave a cache line empty. */
        if (fq->q[(fq->prod_check + fq->line_entries) & fq->entry_mask])
            return -ENOBUFS;
        fq->prod_check += fq->line_entries;
        //__builtin_prefetch(h + fq->line_entries);
    }
#if 0
    assert((((uintptr_t)m) & 0x1) == 0);
#endif
    *h = (uintptr_t)m | ((fq->prod_write >> fq->seqbit_shift) & 0x1);
    fq->prod_write++;
    return 0;
}

static inline int
__iffq_empty(struct iffq *fq, unsigned long i, uintptr_t v)
{
    return (!v) || ((v ^ (i >> fq->seqbit_shift)) & 0x1);
}

/**
 * iffq_empty - test for an empty mailbox
 * @m: the mailbox to test
 *
 * Returns non-zero if the mailbox is empty
 */
static inline int
iffq_empty(struct iffq *m)
{
    uintptr_t v = m->q[m->cons_read & m->entry_mask];

    return __iffq_empty(m, m->cons_read, v);
}

/**
 * iffq_extract - extract a value
 * @fq: the mailbox where to extract from
 *
 * Returns the extracted value, NULL if the mailbox
 * is empty. It does not free up any entry, use
 * iffq_clear for that
 */
static inline struct mbuf *
iffq_extract(struct iffq *fq)
{
    uintptr_t v = fq->q[fq->cons_read & fq->entry_mask];

    if (__iffq_empty(fq, fq->cons_read, v))
        return NULL;

    fq->cons_read++;

    return (struct mbuf *)(v & ~0x1);
}

/**
 * iffq_clear - clear the previously extracted entries
 * @fq: the mailbox to be cleared
 *
 */
static inline void
iffq_clear(struct iffq *fq)
{
    unsigned long s = fq->cons_read & fq->line_mask;

    for (; (fq->cons_clear & fq->line_mask) != s;
         fq->cons_clear += fq->line_entries) {
        fq->q[fq->cons_clear & fq->entry_mask] = 0;
    }
}

static inline void
iffq_prefetch(struct iffq *fq)
{
    __builtin_prefetch((void *)fq->q[fq->cons_read & fq->entry_mask]);
}

static void *
iffq_producer(void *opaque)
{
    struct global *g       = (struct global *)opaque;
    long long int left     = g->num_packets;
    unsigned int pool_mask = g->qlen - 1;
    struct mbuf *pool      = g->pool;
    struct iffq *fq        = g->fq;
    unsigned int pool_idx  = 0;

    runon("P", g->p_core);
    (void)iffq_empty;
    (void)iffq_prefetch;
    iffq_dump("P", fq);

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
        struct mbuf *m = mbuf_get(pool, pool_idx, pool_mask);
#ifdef QDEBUG
        iffq_dump("P", fq);
#endif
        if (iffq_insert(fq, m) == 0) {
            --left;
        } else {
            pool_idx--;
        }
    }
    iffq_dump("P", fq);

    pthread_exit(NULL);
    return NULL;
}

static void *
iffq_consumer(void *opaque)
{
    struct global *g   = (struct global *)opaque;
    long long int left = g->num_packets;
    struct iffq *fq    = g->fq;
    unsigned int sum   = 0;
    struct mbuf *m;
#ifdef RATE
    RATE_HEADER(g);
#endif

    runon("C", g->c_core);
    iffq_dump("C", fq);

    while (left > 0) {
#ifdef QDEBUG
        iffq_dump("C", fq);
#endif
        m = iffq_extract(fq);
        if (m) {
            --left;
            mbuf_put(m, sum);
            iffq_clear(fq);
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    iffq_dump("C", fq);
    printf("[C] sum = %x\n", sum);

    pthread_exit(NULL);
    return NULL;
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

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
    } else if (!strcmp(g->test_type, "iffq")) {
        prod_func = iffq_producer;
        cons_func = iffq_consumer;
    } else {
        printf("Error: unknown test type '%s'\n", g->test_type);
        exit(EXIT_FAILURE);
    }

    if (!strcmp(g->test_type, "msql") || !strcmp(g->test_type, "msq")) {
        g->mq = msq_create(g->qlen, g->batch);
        if (!g->mq) {
            exit(EXIT_FAILURE);
        }
    } else if (!strcmp(g->test_type, "iffq")) {
        g->fq = iffq_create(g->qlen, /*line_size=*/64);
        if (!g->fq) {
            exit(EXIT_FAILURE);
        }
    } else {
        assert(0);
    }
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

    ndiff = (g->end.tv_sec - g->begin.tv_sec) * ONEBILLION +
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
           "    [-c PRODUCER_CORE_ID]\n"
           "    [-c CONSUMER_CORE_ID]\n"
           "    [-t TEST_TYPE (msql,msq,iffq)]\n"
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
    g->num_packets = 10LL * 1000000LL;
    g->qlen        = 256;
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
            g->num_packets = atoi(optarg) * 1000000LL;
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

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    tsc_init();
    run_test(g);

    return 0;
}
