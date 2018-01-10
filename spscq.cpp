/*
 * 2017 Vincenzo Maffione (Universita' di Pisa)
 */
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <ctime>
#include <cerrno>
#include <assert.h>
#include <cstring>
#include <thread>
#include <map>
#include <random>
#include <algorithm>
#include <iostream>
#include <functional>
#include <chrono>
#include <signal.h>

#include "mlib.h"

#undef QDEBUG /* dump queue state at each operation */
#define RATE  /* periodically print rate estimates */

#define HUNDREDMILLIONS (100LL * 1000000LL) /* 100 millions */
#define ONEBILLION (1000LL * 1000000LL)     /* 1 billion */

static int stop = 0;

static void
sigint_handler(int signum)
{
    stop = 1;
    exit(EXIT_SUCCESS);
}

/* Alloc zeroed cacheline-aligned memory, aborting on failure. */
static void *
szalloc(size_t size)
{
    void *p;
    int ret = posix_memalign(&p, CACHELINE_SIZE, size);
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

static unsigned int
ilog2(unsigned int x)
{
    unsigned int probe = 0x00000001U;
    unsigned int ret   = 0;
    unsigned int c;

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
    std::chrono::system_clock::time_point rate_last_ =                         \
        std::chrono::system_clock::now();

#define RATE_BODY(left_)                                                       \
    if (unlikely(left_ < thresh_)) {                                           \
        std::chrono::system_clock::time_point now =                            \
            std::chrono::system_clock::now();                                  \
        double mpps;                                                           \
        mpps = HUNDREDMILLIONS * 1000.0 /                                      \
               std::chrono::duration_cast<std::chrono::nanoseconds>(           \
                   now - rate_last_)                                           \
                   .count();                                                   \
        printf("%3.3f Mpps\n", mpps);                                          \
        thresh_ -= HUNDREDMILLIONS;                                            \
        rate_last_ = now;                                                      \
    }

enum class MbufMode {
    NoAccess = 0,
    LinearAccess,
    ScatteredAccess,
};

struct Mbuf {
    unsigned int len;
    unsigned int __padding[7];
#define MBUF_LEN_MAX 1500
    char buf[MBUF_LEN_MAX];
};

enum class RateLimitMode {
    None = 0,
    Limit,
};

enum class EmulatedOverhead {
    None = 0,
    SpinTSC,
    SpinCycles,
};

struct Blq;
struct Iffq;

struct Global {
    static constexpr int DFLT_N            = 50;
    static constexpr int DFLT_BATCH        = 32;
    static constexpr int DFLT_QLEN         = 256;
    static constexpr int DFLT_LINE_ENTRIES = 32;

    /* Test length as a number of packets. */
    long long int num_packets = DFLT_N * 1000000LL; /* 50 millions */

    /* Length of the SPSC queue. */
    unsigned int qlen = DFLT_QLEN;

    /* How many entries for each line in iffq. */
    unsigned int line_entries = DFLT_LINE_ENTRIES;

    /* Max batch for producer and consumer operation. */
    unsigned int prod_batch = DFLT_BATCH;
    unsigned int cons_batch = DFLT_BATCH;

    /* Affinity for producer and consumer. */
    int p_core = -1, c_core = -1;

    /* Emulated per-packet load for the producer and consumer side,
     * in nanoseconds and ticks. */
    int prod_spin_ns = 0, cons_spin_ns = 0;
    uint64_t prod_spin_cycles = 0, cons_spin_cycles = 0;
    int spin_tsc = 0;

    int cons_rate_limit_ns          = 0;
    uint64_t cons_rate_limit_cycles = 0;

    std::string test_type = "lq";

    MbufMode mbuf_mode = MbufMode::NoAccess;

    /* Timestamp to compute experiment statistics. */
    std::chrono::system_clock::time_point begin, end;

    long long int producer_batches = 0;
    long long int consumer_batches = 0;

    /* The lamport-like queue. */
    Blq *blq = nullptr;

    /* The ff-like queue. */
    Iffq *ffq = nullptr;

    /* A pool of preallocated mbufs. */
    Mbuf *pool = nullptr;

    /* Indirect pool of mbufs, used in MbufMode::ScatteredAccess. */
    Mbuf **spool = nullptr;

    void producer_header();
    void producer_footer();
    void consumer_header();
    void consumer_footer();
};

void
Global::producer_header()
{
    runon("P", p_core);
    begin = std::chrono::system_clock::now();
}

void
Global::producer_footer()
{
    if (producer_batches) {
        printf("[P] avg batch = %.3f\n",
               static_cast<double>(num_packets) /
                   static_cast<double>(producer_batches));
    }
}

void
Global::consumer_header()
{
    runon("C", c_core);
}

void
Global::consumer_footer()
{
    end = std::chrono::system_clock::now();
    if (consumer_batches) {
        printf("[C] avg batch = %.3f\n",
               static_cast<double>(num_packets) /
                   static_cast<double>(consumer_batches));
    }
}

static Mbuf gm;

template <MbufMode kMbufMode>
static inline Mbuf *
mbuf_get(Global *const g, unsigned int *pool_idx, const unsigned int pool_mask)
{
    if (kMbufMode == MbufMode::NoAccess) {
        return &gm;
    } else {
        Mbuf *m;
        if (kMbufMode == MbufMode::LinearAccess) {
            m = &g->pool[*pool_idx & pool_mask];
        } else { /* MbufMode::ScatteredAccess */
            m = g->spool[*pool_idx & pool_mask];
        }
        m->len = (*pool_idx)++;
        return m;
    }
}

template <MbufMode kMbufMode>
static inline void
mbuf_put(Mbuf *const m, unsigned int *sum)
{
    if (kMbufMode != MbufMode::NoAccess) {
        *sum += m->len;
    }
}

/*
 * Multi-section queue, based on the Lamport classic queue.
 * All indices are free running.
 */
typedef Mbuf *blq_entry_t; /* trick to use volatile */
struct Blq {
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
    unsigned int prod_batch;
    unsigned int cons_batch;

    /* The queue. */
    CACHELINE_ALIGNED
    volatile blq_entry_t q[0];
};

static Blq *
blq_create(int qlen, int prod_batch, int cons_batch)
{
    Blq *blq =
        static_cast<Blq *>(szalloc(sizeof(*blq) + qlen * sizeof(blq->q[0])));

    if (qlen < 2 || !is_power_of_two(qlen)) {
        printf("Error: queue length %d is not a power of two\n", qlen);
        return NULL;
    }

    blq->qlen       = qlen;
    blq->qmask      = qlen - 1;
    blq->prod_batch = prod_batch;
    blq->cons_batch = cons_batch;

    assert(reinterpret_cast<uintptr_t>(blq) % CACHELINE_SIZE == 0);
    assert((reinterpret_cast<uintptr_t>(&blq->write)) -
               (reinterpret_cast<uintptr_t>(&blq->write_priv)) ==
           CACHELINE_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->read_priv)) -
               (reinterpret_cast<uintptr_t>(&blq->write)) ==
           CACHELINE_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->read)) -
               (reinterpret_cast<uintptr_t>(&blq->read_priv)) ==
           CACHELINE_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->qlen)) -
               (reinterpret_cast<uintptr_t>(&blq->read)) ==
           CACHELINE_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->q[0])) -
               (reinterpret_cast<uintptr_t>(&blq->qlen)) ==
           CACHELINE_SIZE);

    return blq;
}

static inline unsigned int
blq_wspace(Blq *blq)
{
    return (blq->read - 1 - blq->write_priv) & blq->qmask;
}

/* No boundary checks, to be called after blq_wspace(). */
static inline void
blq_write_local(Blq *blq, Mbuf *m)
{
    blq->q[blq->write_priv & blq->qmask] = m;
    blq->write_priv++;
}

static inline void
blq_write_publish(Blq *blq)
{
    /* Here we need a StoreStore barrier to prevent previous stores to the
     * queue slot and mbuf content to be reordered after the store to
     * blq->write. On x86 a compiler barrier suffices, because stores have
     * release semantic (preventing StoreStore and LoadStore reordering). */
    compiler_barrier();
    blq->write = blq->write_priv;
}

static inline int
blq_write(Blq *blq, Mbuf *m)
{
    if (blq_wspace(blq) == 0) {
        return -1; /* no space */
    }
    blq_write_local(blq, m);
    blq_write_publish(blq);

    return 0;
}

static inline unsigned int
blq_rspace(Blq *blq)
{
    unsigned int space;

    space = blq->write - blq->read_priv;
    /* Here we need a LoadLoad barrier to prevent upcoming loads to the queue
     * slot and mbuf content to be reordered before the load of blq->write. On
     * x86 a compiler barrier suffices, because loads have acquire semantic
     * (preventing LoadLoad and LoadStore reordering). */
    compiler_barrier();

    return space;
}

/* No boundary checks, to be called after blq_rspace(). */
static inline Mbuf *
blq_read_local(Blq *blq)
{
    Mbuf *m = blq->q[blq->read_priv & blq->qmask];
    blq->read_priv++;
    return m;
}

static inline void
blq_read_publish(Blq *blq)
{
    blq->read = blq->read_priv;
}

static inline Mbuf *
blq_read(Blq *blq)
{
    Mbuf *m;

    if (blq_rspace(blq) == 0) {
        return NULL; /* no space */
    }
    m = blq_read_local(blq);
    blq_read_publish(blq);

    return m;
}

static void
blq_dump(const char *prefix, Blq *blq)
{
    printf("[%s] r %u rspace %u w %u wspace %u\n", prefix,
           blq->read & blq->qmask, blq_rspace(blq), blq->write & blq->qmask,
           blq_wspace(blq));
}

static void
blq_free(Blq *blq)
{
    memset(blq, 0, sizeof(*blq));
    free(blq);
}

static inline void
spin_cycles(uint64_t spin)
{
    uint64_t j;
    for (j = 0; j < spin; j++) {
        compiler_barrier();
    }
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
lq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->blq->qmask;
    Blq *const blq               = g->blq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;
#ifdef RATE
    RATE_HEADER(g);
#endif

    assert(blq);
    g->producer_header();
    while (left > 0) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
#ifdef QDEBUG
        blq_dump("P", blq);
#endif
        if (blq_write(blq, m) == 0) {
            --left;
            ++batch_packets;
            if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                tsc_sleep_till(rdtsc() + spin);
            } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            pool_idx--;
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
lq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_cycles;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    long long int left         = g->num_packets;
    Blq *const blq             = g->blq;
    unsigned int sum           = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    assert(blq);
    g->consumer_header();

    while (left > 0) {
#ifdef QDEBUG
        blq_dump("C", blq);
#endif
        m = blq_read(blq);
        if (m) {
            --left;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &sum);
            if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                tsc_sleep_till(rdtsc() + spin);
            } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
        }
    }
    g->consumer_batches = batches;
    g->consumer_footer();
    printf("[C] sum = %x\n", sum);
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
blq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->blq->qmask;
    const unsigned int batch     = g->prod_batch;
    Blq *const blq               = g->blq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;
#ifdef RATE
    RATE_HEADER(g);
#endif

    assert(blq);
    g->producer_header();

    while (left > 0) {
        unsigned int avail = blq_wspace(blq);

#ifdef QDEBUG
        blq_dump("P", blq);
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
            batch_packets += avail;
            for (; avail > 0; avail--) {
                Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
                blq_write_local(blq, m);
                if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                    tsc_sleep_till(rdtsc() + spin);
                } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_cycles(spin);
                }
            }
            blq_write_publish(blq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
blq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_cycles;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    long long int left         = g->num_packets;
    const unsigned int batch   = g->cons_batch;
    Blq *const blq             = g->blq;
    unsigned int sum           = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    assert(blq);
    g->consumer_header();

    while (left > 0) {
        unsigned int avail = blq_rspace(blq);

#ifdef QDEBUG
        blq_dump("C", blq);
#endif
        if (avail) {
            if (avail > batch) {
                avail = batch;
            }
            left -= avail;
            batch_packets += avail;
            for (; avail > 0; avail--) {
                m = blq_read_local(blq);
                mbuf_put<kMbufMode>(m, &sum);
                if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                    tsc_sleep_till(rdtsc() + spin);
                } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_cycles(spin);
                }
            }
            blq_read_publish(blq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
        }
    }
    g->consumer_batches = batches;
    g->consumer_footer();
    printf("[C] sum = %x\n", sum);
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/*
 * FastForward queue.
 * Many fields are only used by the Improved FastFoward queue (see below).
 */
struct Iffq {
    /* Shared (constant) fields. */
    unsigned int entry_mask;
    unsigned int seqbit_shift;
    unsigned int line_entries;
    unsigned int line_mask;

    /* Producer fields. */
    CACHELINE_ALIGNED
    unsigned int prod_write;
    unsigned int prod_check;

    /* Consumer fields. */
    CACHELINE_ALIGNED
    unsigned int cons_clear;
    unsigned int cons_read;

    /* The queue. */
    CACHELINE_ALIGNED
    volatile uintptr_t q[0];
};

static inline int
ffq_write(Iffq *ffq, Mbuf *m)
{
    volatile uintptr_t *qslot = &ffq->q[ffq->prod_write & ffq->entry_mask];

    if (*qslot != 0) {
        return -1; /* no space */
    }
    compiler_barrier();
    *qslot = reinterpret_cast<uintptr_t>(m);
    ffq->prod_write++;

    return 0;
}

static inline Mbuf *
ffq_read(Iffq *ffq)
{
    volatile uintptr_t *qslot = &ffq->q[ffq->cons_read & ffq->entry_mask];
    Mbuf *m                   = reinterpret_cast<Mbuf *>(*qslot);

    if (m != nullptr) {
        *qslot = 0; /* clear */
        ffq->cons_read++;
    }

    compiler_barrier();

    return m;
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
ffq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->ffq->entry_mask;
    Iffq *const ffq              = g->ffq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;
#ifdef RATE
    RATE_HEADER(g);
#endif

    assert(ffq);
    g->producer_header();

    while (left > 0) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
        if (ffq_write(ffq, m) == 0) {
            --left;
            ++batch_packets;
            if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                tsc_sleep_till(rdtsc() + spin);
            } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            pool_idx--;
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
ffq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_cycles;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    long long int left         = g->num_packets;
    Iffq *const ffq            = g->ffq;
    unsigned int sum           = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    assert(ffq);
    g->consumer_header();

    while (left > 0) {
        m = ffq_read(ffq);
        if (m) {
            --left;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &sum);
            if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                tsc_sleep_till(rdtsc() + spin);
            } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
        }
    }
    g->consumer_batches = batches;
    g->consumer_footer();
    printf("[C] sum = %x\n", sum);
}

/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/*
 * Improved FastForward queue, used by pspat.
 */
static inline size_t
iffq_size(unsigned int entries)
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
iffq_init(Iffq *m, unsigned int entries, unsigned int line_size)
{
    unsigned int entries_per_line;

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

    printf("iffq: line_entries %u line_mask %x entry_mask %x seqbit_shift "
           "%u\n",
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
iffq_create(unsigned int entries, unsigned int line_size)
{
    Iffq *ffq;
    int err;

    ffq = static_cast<Iffq *>(szalloc(iffq_size(entries)));
    if (ffq == NULL) {
        return NULL;
    }

    err = iffq_init(ffq, entries, line_size);
    if (err) {
        free(ffq);
        return NULL;
    }

    assert(reinterpret_cast<uintptr_t>(ffq) % CACHELINE_SIZE == 0);
    assert((reinterpret_cast<uintptr_t>(&ffq->cons_clear)) -
               (reinterpret_cast<uintptr_t>(&ffq->prod_write)) ==
           CACHELINE_SIZE);
    assert((reinterpret_cast<uintptr_t>(&ffq->q[0])) -
               (reinterpret_cast<uintptr_t>(&ffq->cons_clear)) ==
           CACHELINE_SIZE);

    return ffq;
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
iffq_dump(const char *prefix, Iffq *ffq)
{
    printf("[%s]: cc %u, cr %u, pw %u, pc %u\n", prefix, ffq->cons_clear,
           ffq->cons_read, ffq->prod_write, ffq->prod_check);
}

/**
 * iffq_insert - enqueue a new value
 * @m: the mailbox where to enqueue
 * @v: the value to be enqueued
 *
 * Returns 0 on success, -ENOBUFS on failure.
 */
static inline int
iffq_insert(Iffq *ffq, Mbuf *m)
{
    volatile uintptr_t *h = &ffq->q[ffq->prod_write & ffq->entry_mask];
    uintptr_t value =
        (uintptr_t)m | ((ffq->prod_write >> ffq->seqbit_shift) & 0x1);

    if (unlikely(ffq->prod_write == ffq->prod_check)) {
        /* Leave a cache line empty. */
        if (ffq->q[(ffq->prod_check + ffq->line_entries) & ffq->entry_mask])
            return -ENOBUFS;
        ffq->prod_check += ffq->line_entries;
        //__builtin_prefetch(h + ffq->line_entries);
    }
#if 0
    assert((((uintptr_t)m) & 0x1) == 0);
#endif
    /* Here we need a StoreStore barrier, to prevent writes to the
     * mbufs to be reordered after the write to the queue slot. */
    compiler_barrier();
    *h = value;
    ffq->prod_write++;
    return 0;
}

static inline unsigned int
iffq_wspace(Iffq *ffq)
{
    if (unlikely(ffq->prod_write == ffq->prod_check)) {
        /* Leave a cache line empty. */
        if (ffq->q[(ffq->prod_check + ffq->line_entries) & ffq->entry_mask])
            return 0;
        ffq->prod_check += ffq->line_entries;
    }
    return ffq->prod_check - ffq->prod_write;
}

static inline void
iffq_insert_nocheck(Iffq *ffq, Mbuf *m)
{
    /* Here we need a StoreStore barrier, to prevent writes to the
     * mbufs to be reordered after the write to the queue slot. */
    compiler_barrier();
    ffq->q[ffq->prod_write & ffq->entry_mask] =
        (uintptr_t)m | ((ffq->prod_write >> ffq->seqbit_shift) & 0x1);
    ffq->prod_write++;
}

static inline int
__iffq_empty(Iffq *ffq, unsigned int i, uintptr_t v)
{
    return (!v) || ((v ^ (i >> ffq->seqbit_shift)) & 0x1);
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
 * @ffq: the mailbox where to extract from
 *
 * Returns the extracted value, NULL if the mailbox
 * is empty. It does not free up any entry, use
 * iffq_clear for that
 */
static inline Mbuf *
iffq_extract(Iffq *ffq)
{
    uintptr_t v = ffq->q[ffq->cons_read & ffq->entry_mask];

    if (__iffq_empty(ffq, ffq->cons_read, v))
        return NULL;

    ffq->cons_read++;

    /* Here we need a LoadLoad barrier, to prevent reads from the
     * mbufs to be reordered before the read to the queue slot. */
    compiler_barrier();

    return (Mbuf *)(v & ~0x1);
}

/**
 * iffq_clear - clear the previously extracted entries
 * @ffq: the mailbox to be cleared
 *
 */
static inline void
iffq_clear(Iffq *ffq)
{
    unsigned int s = ffq->cons_read & ffq->line_mask;

    for (; (ffq->cons_clear /* & ffq->line_mask */) != s;
         ffq->cons_clear += ffq->line_entries) {
        ffq->q[ffq->cons_clear & ffq->entry_mask] = 0;
    }
}

static inline void
iffq_prefetch(Iffq *ffq)
{
    __builtin_prefetch((void *)ffq->q[ffq->cons_read & ffq->entry_mask]);
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
iffq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->qlen - 1;
    Iffq *const ffq              = g->ffq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;
#ifdef RATE
    RATE_HEADER(g);
#endif

    assert(ffq);
    g->producer_header();

    while (left > 0) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
#ifdef QDEBUG
        iffq_dump("P", ffq);
#endif
        if (iffq_insert(ffq, m) == 0) {
            --left;
            ++batch_packets;
            if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                tsc_sleep_till(rdtsc() + spin);
            } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            pool_idx--;
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
iffq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_cycles;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    long long int left         = g->num_packets;
    Iffq *const ffq            = g->ffq;
    unsigned int sum           = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    assert(ffq);
    g->consumer_header();

    while (left > 0) {
#ifdef QDEBUG
        iffq_dump("C", ffq);
#endif
        m = iffq_extract(ffq);
        if (m) {
            --left;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &sum);
            if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                tsc_sleep_till(rdtsc() + spin);
            } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
            iffq_clear(ffq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
        }
    }
    g->consumer_batches = batches;
    g->consumer_footer();
    printf("[C] sum = %x\n", sum);
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
biffq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    long long int left           = g->num_packets;
    const unsigned int pool_mask = g->qlen - 1;
    const unsigned int batch     = g->prod_batch;
    Iffq *const ffq              = g->ffq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;
#ifdef RATE
    RATE_HEADER(g);
#endif

    assert(ffq);
    g->producer_header();

    while (left > 0) {
        unsigned int avail = iffq_wspace(ffq);

#ifdef QDEBUG
        iffq_dump("P", ffq);
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
            batch_packets += avail;
            for (; avail > 0; avail--) {
                Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
                iffq_insert_nocheck(ffq, m);
                if (kEmulatedOverhead == EmulatedOverhead::SpinTSC && spin) {
                    tsc_sleep_till(rdtsc() + spin);
                } else if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_cycles(spin);
                }
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
#ifdef RATE
        RATE_BODY(left);
#endif
    }
    g->producer_batches = batches;
    g->producer_footer();
}

#define biffq_consumer iffq_consumer

/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#if 1
using pc_function_t = void (*)(Global *const);
#else
/* Just using std::function makes the processing loops slower.
 * I cannot believe it! It could be due to some differences in how
 * code is laid out in memory...
 */
using pc_function_t = std::function<void(Global *const)>;
#endif

static int
run_test(Global *g)
{
    std::map<
        std::string,
        std::map<MbufMode,
                 std::map<RateLimitMode,
                          std::map<EmulatedOverhead,
                                   std::pair<pc_function_t, pc_function_t>>>>>
        matrix;
    std::function<uint64_t(uint64_t x)> tf =
        g->spin_tsc ? ns2tsc : [](uint64_t x) { return x; };
    std::pair<pc_function_t, pc_function_t> funcs;
    std::thread pth;
    std::thread cth;
    double mpps;

#define __STRFY(x) #x
#define STRFY(x) __STRFY(x)
#define __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl, eo)                       \
    matrix[STRFY(qname)][mm][rl][eo] = std::make_pair(                         \
        qname##_producer<mm, rl, eo>, qname##_consumer<mm, rl, eo>)
#define __MATRIX_ADD_RATELIMITMODE(qname, mm, rl)                              \
    do {                                                                       \
        __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl, EmulatedOverhead::None);  \
        __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl,                           \
                                      EmulatedOverhead::SpinTSC);              \
        __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl,                           \
                                      EmulatedOverhead::SpinCycles);           \
    } while (0)
#define __MATRIX_ADD_MBUFMODE(qname, mm)                                       \
    do {                                                                       \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::None);            \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::Limit);           \
    } while (0)
#define MATRIX_ADD(qname)                                                      \
    do {                                                                       \
        __MATRIX_ADD_MBUFMODE(qname, MbufMode::NoAccess);                      \
        __MATRIX_ADD_MBUFMODE(qname, MbufMode::LinearAccess);                  \
        __MATRIX_ADD_MBUFMODE(qname, MbufMode::ScatteredAccess);               \
    } while (0)

    /* Multi-section queue (Lamport-like) with legacy operation,
     * i.e. no batching. */
    MATRIX_ADD(lq);
    /* Multi-section queue (Lamport-like) with batching operation. */
    MATRIX_ADD(blq);
    /* Original fast-forward queue. */
    MATRIX_ADD(ffq);
    /* Improved fast-forward queue (PSPAT). */
    MATRIX_ADD(iffq);

    MATRIX_ADD(biffq);

    if (matrix.count(g->test_type) == 0) {
        printf("Error: unknown test type '%s'\n", g->test_type.c_str());
        exit(EXIT_FAILURE);
    }
    RateLimitMode rl =
        g->cons_rate_limit_ns > 0 ? RateLimitMode::Limit : RateLimitMode::None;
    EmulatedOverhead eo = (g->prod_spin_ns == 0 && g->cons_spin_ns == 0)
                              ? EmulatedOverhead::None
                              : (g->spin_tsc ? EmulatedOverhead::SpinTSC
                                             : EmulatedOverhead::SpinCycles);
    funcs = matrix[g->test_type][g->mbuf_mode][rl][eo];

    g->prod_spin_cycles       = tf(g->prod_spin_ns);
    g->cons_spin_cycles       = tf(g->cons_spin_ns);
    g->cons_rate_limit_cycles = tf(g->cons_rate_limit_ns);

    if (g->test_type == "lq" || g->test_type == "blq") {
        g->blq = blq_create(g->qlen, g->prod_batch, g->cons_batch);
        if (!g->blq) {
            exit(EXIT_FAILURE);
        }
        blq_dump("P", g->blq);
    } else if (g->test_type == "iffq" || g->test_type == "ffq" ||
               g->test_type == "biffq") {
        (void)iffq_empty;
        (void)iffq_prefetch;
        g->ffq = iffq_create(g->qlen,
                             /*line_size=*/g->line_entries * sizeof(uintptr_t));
        if (!g->ffq) {
            exit(EXIT_FAILURE);
        }
        iffq_dump("P", g->ffq);
    } else {
        assert(0);
    }

    /* Allocate mbuf pool. */
    g->pool = static_cast<Mbuf *>(szalloc(g->qlen * sizeof(g->pool[0])));

    if (g->mbuf_mode == MbufMode::ScatteredAccess) {
        /* Prepare support for scattered mbufs. First create a vector
         * of qlen elements, containing a random permutation of
         * [0..qlen[. */
        std::vector<int> v(g->qlen);
        std::random_device rd;
        std::mt19937 gen(rd());

        for (size_t i = 0; i < v.size(); i++) {
            v[i] = i;
        }
        std::shuffle(v.begin(), v.end(), gen);

        /* Then allocate and initialize g->spool, whose slots points at the
         * mbufs in the pool following the random pattern. */
        g->spool =
            static_cast<Mbuf **>(szalloc(v.size() * sizeof(g->spool[0])));
        for (size_t i = 0; i < v.size(); i++) {
            g->spool[i] = g->pool + v[i];
        }
    }

    pth = std::thread(funcs.first, g);
    cth = std::thread(funcs.second, g);
    pth.join();
    cth.join();

    mpps =
        g->num_packets * 1000.0 /
        std::chrono::duration_cast<std::chrono::nanoseconds>(g->end - g->begin)
            .count();
    printf("Throughput %3.3f Mpps\n", mpps);

    free(g->pool);
    if (g->blq) {
        blq_dump("P", g->blq);
        blq_free(g->blq);
        g->blq = nullptr;
    }
    if (g->ffq) {
        iffq_dump("P", g->ffq);
        iffq_free(g->ffq);
        g->ffq = nullptr;
    }

    return 0;
}

static void
usage(const char *progname)
{
    printf("%s [-h]\n"
           "    [-n NUM_PACKETS (in millions) = %d]\n"
           "    [-b MAX_PRODUCER_BATCH = %d]\n"
           "    [-b MAX_CONSUMER_BATCH = %d]\n"
           "    [-l QUEUE_LENGTH = %d]\n"
           "    [-L LINE_ENTRIES (iffq) = %d]\n"
           "    [-c PRODUCER_CORE_ID = -1]\n"
           "    [-c CONSUMER_CORE_ID = -1]\n"
           "    [-P PRODUCER_SPIN_NS = 0]\n"
           "    [-C CONSUMER_SPIN_NS = 0]\n"
           "    [-t TEST_TYPE (lq,blq,ffq,iffq,biffq)]\n"
           "    [-M (access mbuf content)]\n"
           "    [-r CONSUMER_RATE_LIMIT_NS = 0]\n"
           "\n",
           progname, Global::DFLT_N, Global::DFLT_BATCH, Global::DFLT_BATCH,
           Global::DFLT_QLEN, Global::DFLT_LINE_ENTRIES);
}

int
main(int argc, char **argv)
{
    bool got_b_option = false;
    struct sigaction sa;
    int opt, ret;
    Global _g;
    Global *g = &_g;

    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret         = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }

    while ((opt = getopt(argc, argv, "hn:b:l:c:t:L:P:C:Mr:T")) != -1) {
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
            if (!got_b_option) {
                got_b_option  = true;
                g->prod_batch = atoi(optarg);
                if (g->prod_batch < 1) {
                    printf("    Invalid producer batch '%s'\n", optarg);
                    return -1;
                }
            } else {
                g->cons_batch = atoi(optarg);
                if (g->cons_batch < 1) {
                    printf("    Invalid consumer batch '%s'\n", optarg);
                    return -1;
                }
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

        case 'T':
            g->spin_tsc = 1;
            break;

        case 'M':
            switch (g->mbuf_mode) {
            case MbufMode::NoAccess:
                g->mbuf_mode = MbufMode::LinearAccess;
                break;
            case MbufMode::LinearAccess:
            case MbufMode::ScatteredAccess:
                g->mbuf_mode = MbufMode::ScatteredAccess;
                break;
            }
            break;

        case 'r':
            g->cons_rate_limit_ns = atoi(optarg);
            if (g->cons_rate_limit_ns < 0) {
                printf("    Invalid consumer rate limit '%s'\n", optarg);
                return -1;
            }
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
