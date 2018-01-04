/*
 * 2017 Vincenzo Maffione (Universita' di Pisa)
 */
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <thread>
#include <map>

#include "mlib.h"

#undef QDEBUG /* dump queue state at each operation */
#define RATE  /* periodically print rate estimates */

#define HUNDREDMILLIONS (100LL * 1000000LL) /* 100 millions */
#define ONEBILLION (1000LL * 1000000LL)     /* 1 billion */

/* Alloc zeroed memory, aborting on failure. */
static void *
szalloc(size_t size)
{
    void *p;
    int ret = posix_memalign(&p, CACHE_SIZE, size);
    if (ret) {
        printf("allocation failure: %s\n", strerror(errno));
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

struct Mbuf {
    unsigned int len;
    unsigned int __padding[7];
#define MBUF_LEN_MAX 1500
    char buf[MBUF_LEN_MAX];
};

static Mbuf gm;
#define mbuf_get(pool_, pool_idx_, pool_mask_)                                 \
    ({                                                                         \
        Mbuf *m = &pool[pool_idx_ & pool_mask_];                        \
        m->len         = pool_idx_++;                                          \
        m;                                                                     \
    })

#define mbuf_put(m_, sum_) sum_ += m_->len

enum class MbufMode {
    NoAccess = 0,
    OneAccess,
};

struct Global;
struct Msq;
struct Iffq;
typedef void (*pc_function_t)(Global *const);

struct Global {
    static constexpr int DFLT_N = 50;
    static constexpr int DFLT_BATCH  = 32;
    static constexpr int DFLT_QLEN  = 256;
    static constexpr int DFLT_LINE_ENTRIES = 8;

    /* Test length as a number of packets. */
    long long int num_packets = DFLT_N * 1000000LL; /* 50 millions */;

    /* Length of the SPSC queue. */
    unsigned int qlen = DFLT_QLEN;

    /* How many entries for each line in iffq. */
    unsigned int line_entries = DFLT_LINE_ENTRIES;

    /* Max batch for producer and consumer operation. */
    unsigned int batch = DFLT_BATCH;

    /* Affinity for producer and consumer. */
    int p_core = -1, c_core = -1;

    /* Emulated per-packet load for the producer and consumer side,
     * in nanoseconds and ticks. */
    int prod_spin_ns = 0, cons_spin_ns = 0;
    uint64_t prod_spin_ticks = 0, cons_spin_ticks = 0;

    std::string test_type = "msql";

    MbufMode mbuf_mode = MbufMode::NoAccess;

    /* Timestamp to compute experiment statistics. */
    struct timespec begin, end;

    /* The lamport-like queue. */
    Msq *mq = nullptr;

    /* The ff-like queue. */
    Iffq *fq = nullptr;

    /* A pool of preallocated mbufs. */
    Mbuf *pool = nullptr;
};

/*
 * Multi-section queue, based on the Lamport classic queue.
 * All indices are free running.
 */
typedef Mbuf *msq_entry_t; /* trick to use volatile */
struct Msq {
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

static Msq *
msq_create(int qlen, int batch)
{
    Msq *mq = static_cast<Msq *>(szalloc(sizeof(*mq) + qlen * sizeof(mq->q[0])));

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
msq_wspace(Msq *mq)
{
    return (mq->read - 1 - mq->write_priv) & mq->qmask;
}

/* No boundary checks, to be called after msq_wspace(). */
static inline void
msq_write_local(Msq *mq, Mbuf *m)
{
    mq->q[mq->write_priv & mq->qmask] = m;
    mq->write_priv++;
}

static inline void
msq_write_publish(Msq *mq)
{
    /* Here we need a StoreStore barrier to prevent previous stores to the
     * queue slot and mbuf content to be reordered after the store to
     * mq->write. On x86 a compiler barrier suffices, because stores have
     * release semantic (preventing StoreStore and LoadStore reordering). */
    compiler_barrier();
    mq->write = mq->write_priv;
}

static inline int
msq_write(Msq *mq, Mbuf *m)
{
    if (msq_wspace(mq) == 0) {
        return -1; /* no space */
    }
    msq_write_local(mq, m);
    msq_write_publish(mq);

    return 0;
}

static inline unsigned int
msq_rspace(Msq *mq)
{
    unsigned int space;

    space = mq->write - mq->read_priv;
    /* Here we need a LoadLoad barrier to prevent upcoming loads to the queue
     * slot and mbuf content to be reordered before the load of mq->write. On
     * x86 a compiler barrier suffices, because loads have acquire semantic
     * (preventing LoadLoad and LoadStore reordering). */
    compiler_barrier();

    return space;
}

/* No boundary checks, to be called after msq_rspace(). */
static inline Mbuf *
msq_read_local(Msq *mq)
{
    Mbuf *m = mq->q[mq->read_priv & mq->qmask];
    mq->read_priv++;
    return m;
}

static inline void
msq_read_publish(Msq *mq)
{
    mq->read = mq->read_priv;
}

static inline Mbuf *
msq_read(Msq *mq)
{
    Mbuf *m;

    if (msq_rspace(mq) == 0) {
        return NULL; /* no space */
    }
    m = msq_read_local(mq);
    msq_read_publish(mq);

    return m;
}

static void
msq_dump(const char *prefix, Msq *mq)
{
    printf("[%s] r %u rspace %u w %u wspace %u\n", prefix, mq->read & mq->qmask,
           msq_rspace(mq), mq->write & mq->qmask, msq_wspace(mq));
}

static void
msq_free(Msq *mq)
{
    memset(mq, 0, sizeof(*mq));
    free(mq);
}

template<MbufMode kMbufMode>
static void
msq_legacy_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_ticks;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->mq->qmask;
    Mbuf *const pool      = g->pool;
    Msq *const mq         = g->mq;
    unsigned int pool_idx        = 0;

    runon("P", g->p_core);

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
        Mbuf *m;
        if (kMbufMode == MbufMode::NoAccess) {
            m = &gm;
        } else {
            m = mbuf_get(pool, pool_idx, pool_mask);
        }
#ifdef QDEBUG
        msq_dump("P", mq);
#endif
        if (msq_write(mq, m) == 0) {
            --left;
            if (spin) {
                tsc_sleep_till(rdtsc() + spin);
            }
        } else {
            pool_idx--;
        }
    }
    msq_dump("P", mq);
}

template<MbufMode kMbufMode>
static void
msq_legacy_consumer(Global *const g)
{
    const uint64_t spin    = g->cons_spin_ticks;
    long long int left     = g->num_packets;
    Msq *const mq   = g->mq;
    unsigned int sum       = 0;
    Mbuf *m;
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
            if (kMbufMode == MbufMode::OneAccess) {
            mbuf_put(m, sum);
            }
            if (spin) {
                tsc_sleep_till(rdtsc() + spin);
            }
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    msq_dump("C", mq);
    printf("[C] sum = %x\n", sum);
}

template<MbufMode kMbufMode>
static void
msq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_ticks;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->mq->qmask;
    const unsigned int batch     = g->batch;
    Mbuf *const pool      = g->pool;
    Msq *const mq         = g->mq;
    unsigned int pool_idx        = 0;

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
                Mbuf *m;
                if (kMbufMode == MbufMode::NoAccess) {
                    m = &gm;
                } else {
                    m = mbuf_get(pool, pool_idx, pool_mask);
                }
                msq_write_local(mq, m);
                if (spin) {
                    tsc_sleep_till(rdtsc() + spin);
                }
            }
            msq_write_publish(mq);
        }
    }
    msq_dump("P", mq);
}

template<MbufMode kMbufMode>
static void
msq_consumer(Global *const g)
{
    const uint64_t spin      = g->cons_spin_ticks;
    long long int left       = g->num_packets;
    const unsigned int batch = g->batch;
    Msq *const mq     = g->mq;
    unsigned int sum         = 0;
    Mbuf *m;
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
                if (kMbufMode == MbufMode::OneAccess) {
                    mbuf_put(m, sum);
                }
                if (spin) {
                    tsc_sleep_till(rdtsc() + spin);
                }
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
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/*
 * Improved FastForward queue, used by pspat.
 */
struct Iffq {
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
    return roundup(sizeof(Iffq) + entries * sizeof(uintptr_t), 64);
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
iffq_init(Iffq *m, unsigned long entries, unsigned long line_size)
{
    unsigned long entries_per_line;

    if (!is_power_of_two(entries) || !is_power_of_two(line_size) ||
        entries * sizeof(uintptr_t) <= 2 * line_size ||
        line_size < sizeof(uintptr_t)) {
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
Iffq *
iffq_create(unsigned long entries, unsigned long line_size)
{
    Iffq *m;
    int err;

    m = static_cast<Iffq*>(szalloc(iffq_size(entries)));
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
iffq_free(Iffq *m)
{
    free(m);
}

void
iffq_dump(const char *prefix, Iffq *fq)
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
iffq_insert(Iffq *fq, Mbuf *m)
{
    volatile uintptr_t *h = &fq->q[fq->prod_write & fq->entry_mask];
    uintptr_t value =
        (uintptr_t)m | ((fq->prod_write >> fq->seqbit_shift) & 0x1);

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
    /* Here we need a StoreStore barrier, to prevent writes to the
     * mbufs to be reordered after the write to the queue slot. */
    compiler_barrier();
    *h = value;
    fq->prod_write++;
    return 0;
}

static inline int
__iffq_empty(Iffq *fq, unsigned long i, uintptr_t v)
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
iffq_empty(Iffq *m)
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
static inline Mbuf *
iffq_extract(Iffq *fq)
{
    uintptr_t v = fq->q[fq->cons_read & fq->entry_mask];

    if (__iffq_empty(fq, fq->cons_read, v))
        return NULL;

    fq->cons_read++;

    /* Here we need a LoadLoad barrier, to prevent reads from the
     * mbufs to be reordered before the read to the queue slot. */
    compiler_barrier();

    return (Mbuf *)(v & ~0x1);
}

/**
 * iffq_clear - clear the previously extracted entries
 * @fq: the mailbox to be cleared
 *
 */
static inline void
iffq_clear(Iffq *fq)
{
    unsigned long s = fq->cons_read & fq->line_mask;

    for (; (fq->cons_clear /* & fq->line_mask */) != s;
         fq->cons_clear += fq->line_entries) {
        fq->q[fq->cons_clear & fq->entry_mask] = 0;
    }
}

static inline void
iffq_prefetch(Iffq *fq)
{
    __builtin_prefetch((void *)fq->q[fq->cons_read & fq->entry_mask]);
}

template<MbufMode kMbufMode>
static void
iffq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_ticks;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->qlen - 1;
    Mbuf *const pool      = g->pool;
    Iffq *const fq        = g->fq;
    unsigned int pool_idx        = 0;

    runon("P", g->p_core);
    (void)iffq_empty;
    (void)iffq_prefetch;
    iffq_dump("P", fq);

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
        Mbuf *m;
        if (kMbufMode == MbufMode::NoAccess) {
            m = &gm;
        } else {
            m = mbuf_get(pool, pool_idx, pool_mask);
        }
#ifdef QDEBUG
        iffq_dump("P", fq);
#endif
        if (iffq_insert(fq, m) == 0) {
            --left;
            if (spin) {
                tsc_sleep_till(rdtsc() + spin);
            }
        } else {
            pool_idx--;
        }
    }
    iffq_dump("P", fq);
}

template<MbufMode kMbufMode>
static void
iffq_consumer(Global *const g)
{
    const uint64_t spin    = g->cons_spin_ticks;
    long long int left     = g->num_packets;
    Iffq *const fq  = g->fq;
    unsigned int sum       = 0;
    Mbuf *m;
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
            if (kMbufMode == MbufMode::OneAccess) {
                mbuf_put(m, sum);
            }
            if (spin) {
                tsc_sleep_till(rdtsc() + spin);
            }
            iffq_clear(fq);
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    iffq_dump("C", fq);
    printf("[C] sum = %x\n", sum);
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

static int
run_test(Global *g)
{
    std::map<std::string, std::map<MbufMode, std::pair<pc_function_t, pc_function_t>>> matrix;
    std::pair<pc_function_t, pc_function_t> funcs;
    unsigned long int ndiff;
    double mpps;

    /* Multi-section queue (Lamport-like) with legacy operation,
     * i.e. no batching. */
    matrix["msql"][MbufMode::NoAccess] = std::make_pair(msq_legacy_producer<MbufMode::NoAccess>, msq_legacy_consumer<MbufMode::NoAccess>);
    matrix["msql"][MbufMode::OneAccess] = std::make_pair(msq_legacy_producer<MbufMode::OneAccess>, msq_legacy_consumer<MbufMode::OneAccess>);
        /* Multi-section queue (Lamport-like) with batching operation. */
    matrix["msq"][MbufMode::NoAccess] = std::make_pair(msq_producer<MbufMode::NoAccess>, msq_consumer<MbufMode::NoAccess>);
    matrix["msq"][MbufMode::OneAccess] = std::make_pair(msq_producer<MbufMode::OneAccess>, msq_consumer<MbufMode::OneAccess>);
        /* Improved fast-forward queue (PSPAT). */
    matrix["iffq"][MbufMode::NoAccess] = std::make_pair(iffq_producer<MbufMode::NoAccess>, iffq_consumer<MbufMode::NoAccess>);
    matrix["iffq"][MbufMode::OneAccess] = std::make_pair(iffq_producer<MbufMode::OneAccess>, iffq_consumer<MbufMode::OneAccess>);

    if (matrix.count(g->test_type) == 0) {
        printf("Error: unknown test type '%s'\n", g->test_type.c_str());
        exit(EXIT_FAILURE);
    }
    funcs = matrix[g->test_type][g->mbuf_mode];

    g->prod_spin_ticks = ns2tsc(g->prod_spin_ns);
    g->cons_spin_ticks = ns2tsc(g->cons_spin_ns);

    if (g->test_type == "msql" || g->test_type == "msq") {
        g->mq = msq_create(g->qlen, g->batch);
        if (!g->mq) {
            exit(EXIT_FAILURE);
        }
    } else if (g->test_type == "iffq") {
        g->fq = iffq_create(g->qlen,
                            /*line_size=*/g->line_entries * sizeof(uintptr_t));
        if (!g->fq) {
            exit(EXIT_FAILURE);
        }
    } else {
        assert(0);
    }
    g->pool = static_cast<Mbuf *>(malloc(g->qlen * sizeof(g->pool[0])));

    std::thread pth(funcs.first, g);
    std::thread cth(funcs.second, g);
    pth.join();
    cth.join();

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
           "    [-n NUM_PACKETS (in millions) = %d]\n"
           "    [-b MAX_BATCH = %d]\n"
           "    [-l QUEUE_LENGTH = %d]\n"
           "    [-L LINE_ENTRIES (iffq) = %d]\n"
           "    [-c PRODUCER_CORE_ID = -1]\n"
           "    [-c CONSUMER_CORE_ID = -1]\n"
           "    [-P PRODUCER_SPIN_NS = 0]\n"
           "    [-C CONSUMER_SPIN_NS = 0]\n"
           "    [-t TEST_TYPE (msql,msq,iffq)]\n"
           "    [-M (access mbuf content)]\n"
           "\n",
           progname, Global::DFLT_N, Global::DFLT_BATCH, Global::DFLT_QLEN, Global::DFLT_LINE_ENTRIES);
}

int
main(int argc, char **argv)
{
    Global _g;
    Global *g = &_g;
    int opt;

    while ((opt = getopt(argc, argv, "hn:b:l:c:t:L:P:C:M")) != -1) {
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
            if (g->num_packets == 0) {
                g->num_packets = (1ULL << 63) - 1;
            }
            break;

        case 'b':
            g->batch = atoi(optarg);
            if (g->batch < 1) {
                printf("    Invalid batch '%s'\n", optarg);
                return -1;
            }
            break;

        case 'l':
            g->qlen = atoi(optarg);
            if (g->qlen < 2 || !is_power_of_two(g->qlen)) {
                printf(
                    "    Invalid queue length '%s' (must be a power of two)\n",
                    optarg);
                return -1;
            }
            break;

        case 'L':
            g->line_entries = atoi(optarg);
            if (g->line_entries < 8 || !is_power_of_two(g->line_entries)) {
                printf(
                    "    Invalid line entries '%s' (must be a power of two)\n",
                    optarg);
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
            g->test_type = std::string(optarg);
            break;

        case 'P':
            g->prod_spin_ns = atoi(optarg);
            if (g->prod_spin_ns < 0) {
                printf("    Invalid producer spin '%s'\n", optarg);
                return -1;
            }
            break;

        case 'C':
            g->cons_spin_ns = atoi(optarg);
            if (g->cons_spin_ns < 0) {
                printf("    Invalid consumer spin '%s'\n", optarg);
                return -1;
            }
            break;

        case 'M':
            g->mbuf_mode = MbufMode::OneAccess;
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
