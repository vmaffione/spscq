/*
 * 2017 Vincenzo Maffione (Universita' di Pisa)
 */
#include <stdio.h>
#include <cstdlib>
#include <fstream>
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
#include <sstream>

#include "mlib.h"

#undef QDEBUG /* dump queue state at each operation */

#define ONEBILLION (1000LL * 1000000LL) /* 1 billion */

static volatile int stop = 0;

static void
sigint_handler(int signum)
{
    stop = 1;
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

unsigned int
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

struct RateLimitedStats {
    std::chrono::system_clock::time_point last;
    long long int step   = 30ULL * 1000000ULL;
    long long int thresh = 0;

    RateLimitedStats(long long int th)
        : last(std::chrono::system_clock::now()), thresh(th - step)
    {
    }

    inline void stat(long long int left)
    {
        if (unlikely(left < thresh)) {
            std::chrono::system_clock::time_point now =
                std::chrono::system_clock::now();
            long long unsigned int ndiff =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - last)
                    .count();
            double mpps;
            mpps = step * 1000.0 / ndiff;
            printf("%3.3f Mpps\n", mpps);
            if (ndiff < 2 * ONEBILLION) {
                step <<= 1;
            } else if (ndiff > 3 * ONEBILLION) {
                step >>= 1;
            }
            thresh -= step;
            last = now;
        }
    }
};

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
    SpinCycles,
};

struct Blq;
struct Iffq;

struct Global {
    static constexpr int DFLT_BATCH        = 32;
    static constexpr int DFLT_QLEN         = 256;
    static constexpr int DFLT_LINE_ENTRIES = 32;
    static constexpr int DFLT_D            = 10;

    /* Test length as a number of packets. */
    long long unsigned int num_packets = 0; /* infinite */

    /* Length of the SPSC queue. */
    unsigned int qlen = DFLT_QLEN;

    /* How many entries for each line in iffq. */
    unsigned int line_entries = DFLT_LINE_ENTRIES;

    /* Max batch for producer and consumer operation. */
    unsigned int prod_batch = DFLT_BATCH;
    unsigned int cons_batch = DFLT_BATCH;

    /* Affinity for producer and consumer. */
    int p_core = 0, c_core = 1;

    /* Emulated per-packet load for the producer and consumer side,
     * in nanoseconds and ticks. */
    uint64_t prod_spin_cycles = 0, cons_spin_cycles = 0;
    uint64_t cons_rate_limit_cycles = 0;

    bool online_rate   = false;
    bool perf_counters = false;

    /* Test duration in seconds. */
    unsigned int duration = DFLT_D;

    /* Type of queue used. */
    std::string test_type = "lq";

    /* If true we do a latency test; if false we do a throughput test. */
    bool latency = false;

    MbufMode mbuf_mode = MbufMode::NoAccess;

    /* Timestamp to compute experiment statistics. */
    std::chrono::system_clock::time_point begin, end;

    /* Checksum for when -M is used. */
    unsigned int csum;

    /* Packet count written back by consumers. It's safer for it
     * to have its own cacheline. */
    CACHELINE_ALIGNED
    volatile long long unsigned pkt_cnt = 0;

    /* Average batches as seen by producer and consumer. */
    CACHELINE_ALIGNED
    long long int producer_batches = 0;
    long long int consumer_batches = 0;

    /* L1 dcache miss rate in M/sec. */
    float prod_miss_rate = 0.0;
    float cons_miss_rate = 0.0;

    /* CPU instruction rate in B/sec. */
    float prod_insn_rate = 0.0;
    float cons_insn_rate = 0.0;

    /* The lamport-like queue. */
    Blq *blq      = nullptr;
    Blq *blq_back = nullptr;

    /* The ff-like queue. */
    Iffq *ffq      = nullptr;
    Iffq *ffq_back = nullptr;

    /* A pool of preallocated mbufs. */
    Mbuf *pool = nullptr;

    /* Indirect pool of mbufs, used in MbufMode::ScatteredAccess. */
    Mbuf **spool = nullptr;

    void producer_header();
    void producer_footer();
    void consumer_header();
    void consumer_footer();
    void print_results();
};

static void
miss_rate_print(const char *prefix, double mpps, float miss_rate)
{
    double ratio     = mpps / miss_rate;
    const char *unit = "packets/miss";

    if (ratio < 1.0) {
        ratio = 1 / ratio;
        unit  = "misses/packet";
    }

    printf("[%s] L1 d-cache miss rate %5.2f M/sec, %.2f %s\n", prefix,
           miss_rate, ratio, unit);
}

void
Global::print_results()
{
    double mpps =
        pkt_cnt * 1000.0 /
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count();

    if (csum) {
        unsigned int expect = static_cast<unsigned int>(
            static_cast<long long unsigned>(pkt_cnt) * (pkt_cnt - 1) / 2);
        // printf("PKTS %llu\n", pkt_cnt);
        printf("[C] csum = %x, expect = %x, diff = %x\n", csum, expect,
               expect - csum);
    }

    if (prod_insn_rate != 0.0) {
        printf("[P] %.2f Ginsn/s %.2f CPU insn per packet\n", prod_insn_rate,
               prod_insn_rate * 1000.0 / mpps);
    }
    if (cons_insn_rate != 0.0) {
        printf("[C] %.2f Ginsn/s %.2f CPU insn per packet\n", cons_insn_rate,
               cons_insn_rate * 1000.0 / mpps);
    }
    if (producer_batches) {
        printf("[P] avg batch = %.3f\n",
               static_cast<double>(pkt_cnt) /
                   static_cast<double>(producer_batches));
    }
    if (consumer_batches) {
        printf("[C] avg batch = %.3f\n",
               static_cast<double>(pkt_cnt) /
                   static_cast<double>(consumer_batches));
    }
    miss_rate_print("P", mpps, prod_miss_rate);
    miss_rate_print("C", mpps, cons_miss_rate);
    printf("Throughput %3.3f Mpps\n", mpps);
}

void
Global::producer_header()
{
    runon("P", p_core);
    begin = std::chrono::system_clock::now();
}

void
Global::producer_footer()
{
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
mbuf_put(Mbuf *const m, unsigned int *csum)
{
    if (kMbufMode != MbufMode::NoAccess) {
        *csum += m->len;
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
    unsigned int read_shadow;

    /* Producer write, consumer read. */
    CACHELINE_ALIGNED
    volatile unsigned int write;

    /* Consumer private data. */
    CACHELINE_ALIGNED
    unsigned int read_priv;
    unsigned int write_shadow;

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

//#define LLQ
static inline int
lq_write(Blq *q, Mbuf *m)
{
    unsigned int next = (q->write + 1) & q->qmask;

#ifdef LLQ
    if (next == q->read_shadow) {
        q->read_shadow = q->read;
    }
    if (next == q->read_shadow) {
        return -1; /* no space */
    }
#else
    if (next == q->read) {
        return -1; /* no space */
    }
#endif
    q->q[q->write] = m;
    compiler_barrier();
    q->write = next;
    return 0;
}

static inline Mbuf *
lq_read(Blq *q)
{
    Mbuf *m;
#ifdef LLQ
    if (q->read == q->write_shadow) {
        q->write_shadow = q->write;
    }
    if (q->read == q->write_shadow) {
        return NULL; /* queue empty */
    }
#else
    if (q->read == q->write) {
        return NULL; /* queue empty */
    }
#endif
    compiler_barrier();
    m       = q->q[q->read];
    q->read = (q->read + 1) & q->qmask;
    return m;
}

static inline unsigned int
blq_wspace(Blq *blq)
{
    unsigned int space = (blq->read_shadow - 1 - blq->write_priv) & blq->qmask;

    if (space) {
        return space;
    }
    blq->read_shadow = blq->read;

    return (blq->read_shadow - 1 - blq->write_priv) & blq->qmask;
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

static inline unsigned int
blq_rspace(Blq *blq)
{
    unsigned int space = blq->write_shadow - blq->read_priv;

    if (space) {
        return space;
    }
    blq->write_shadow = blq->write;
    /* Here we need a LoadLoad barrier to prevent upcoming loads to the queue
     * slot and mbuf content to be reordered before the load of blq->write. On
     * x86 a compiler barrier suffices, because loads have acquire semantic
     * (preventing LoadLoad and LoadStore reordering). */
    compiler_barrier();

    return blq->write_shadow - blq->read_priv;
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
    const unsigned int pool_mask = g->blq->qmask;
    Blq *const blq               = g->blq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;

    g->producer_header();
    while (!stop) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
#ifdef QDEBUG
        blq_dump("P", blq);
#endif
        if (lq_write(blq, m) == 0) {
            ++batch_packets;
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            pool_idx--;
        }
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
    Blq *const blq             = g->blq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    g->consumer_header();
    for (;;) {
#ifdef QDEBUG
        blq_dump("C", blq);
#endif
        m = lq_read(blq);
        if (m) {
            ++g->pkt_cnt;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &csum);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(stop)) {
                break;
            }
        }
    }
    g->consumer_batches = batches;
    g->csum             = csum;
    g->consumer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
blq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    const unsigned int pool_mask = g->blq->qmask;
    const unsigned int batch     = g->prod_batch;
    Blq *const blq               = g->blq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;

    g->producer_header();

    while (!stop) {
        unsigned int avail = blq_wspace(blq);

#ifdef QDEBUG
        blq_dump("P", blq);
#endif
        if (avail) {
            if (avail > batch) {
                avail = batch;
            }
            batch_packets += avail;
            for (; avail > 0; avail--) {
                Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
                blq_write_local(blq, m);
                if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_cycles(spin);
                }
            }
            blq_write_publish(blq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
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
    const unsigned int batch   = g->cons_batch;
    Blq *const blq             = g->blq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    g->consumer_header();

    for (;;) {
        unsigned int avail = blq_rspace(blq);

#ifdef QDEBUG
        blq_dump("C", blq);
#endif
        if (avail) {
            if (avail > batch) {
                avail = batch;
            }
            batch_packets += avail;
            g->pkt_cnt += avail;
            for (; avail > 0; avail--) {
                m = blq_read_local(blq);
                mbuf_put<kMbufMode>(m, &csum);
                if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
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
            if (unlikely(stop)) {
                break;
            }
        }
    }
    g->csum             = csum;
    g->consumer_batches = batches;
    g->consumer_footer();
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/*
 * FastForward queue.
 * Many fields are only used by the Improved FastFoward queue (see below).
 */
struct Iffq {
#define IFFQ_PROD_CACHE_ENTRIES 256
    uintptr_t prod_cache[IFFQ_PROD_CACHE_ENTRIES];

    CACHELINE_ALIGNED
    /* Shared (constant) fields. */
    unsigned int entry_mask;
    unsigned int line_entries;
    unsigned int line_mask;

    /* Producer fields. */
    CACHELINE_ALIGNED
    unsigned int prod_write;
    unsigned int prod_check;
    unsigned int prod_write_pub;
    unsigned int prod_cache_write;

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

    return m;
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
ffq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    const unsigned int pool_mask = g->ffq->entry_mask;
    Iffq *const ffq              = g->ffq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;

    g->producer_header();

    while (!stop) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
        if (kMbufMode != MbufMode::NoAccess) {
            compiler_barrier();
        }
        if (ffq_write(ffq, m) == 0) {
            ++batch_packets;
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            pool_idx--;
        }
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
    Iffq *const ffq            = g->ffq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    g->consumer_header();

    for (;;) {
        m = ffq_read(ffq);
        if (m) {
            ++batch_packets;
            ++g->pkt_cnt;
            mbuf_put<kMbufMode>(m, &csum);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(stop)) {
                break;
            }
        }
    }
    g->csum             = csum;
    g->consumer_batches = batches;
    g->consumer_footer();
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
iffq_init(Iffq *m, unsigned int entries, unsigned int line_size, bool improved)
{
    unsigned int entries_per_line;
    unsigned int i;

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

    printf("iffq: line_entries %u line_mask %x entry_mask %x\n",
           m->line_entries, m->line_mask, m->entry_mask);

    m->cons_clear = 0;
    m->cons_read  = m->line_entries;
    m->prod_write = m->prod_write_pub = m->line_entries;
    m->prod_check                     = 2 * m->line_entries;
    m->prod_cache_write               = 0;

    if (improved) {
        /* For iffq and biffq we need to have something different
         * from nullptr in [cons_clear, cons_read[, or the producer
         * can get confused. */
        for (i = m->cons_clear; i != m->cons_read; i++) {
            m->q[i] = (uintptr_t)1; /* garbage */
        }
    }

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
__iffq_create(unsigned int entries, unsigned int line_size, bool improved)
{
    Iffq *ffq;
    int err;

    ffq = static_cast<Iffq *>(szalloc(iffq_size(entries)));
    if (ffq == NULL) {
        return NULL;
    }

    err = iffq_init(ffq, entries, line_size, improved);
    if (err) {
        free(ffq);
        return NULL;
    }

    assert(reinterpret_cast<uintptr_t>(ffq) % CACHELINE_SIZE == 0);
    assert(((reinterpret_cast<uintptr_t>(&ffq->cons_clear)) -
            (reinterpret_cast<uintptr_t>(&ffq->prod_write))) %
               CACHELINE_SIZE ==
           0);
    assert((reinterpret_cast<uintptr_t>(&ffq->q[0])) -
               (reinterpret_cast<uintptr_t>(&ffq->cons_clear)) ==
           CACHELINE_SIZE);

    return ffq;
}

Iffq *
iffq_create(unsigned int entries, unsigned int line_size)
{
    return __iffq_create(entries, line_size, /*improved=*/true);
}

Iffq *
ffq_create(unsigned int entries, unsigned int line_size)
{
    return __iffq_create(entries, line_size, /*improved=*/false);
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
    if (unlikely(ffq->prod_write == ffq->prod_check)) {
        /* Leave a cache line empty. */
        if (ffq->q[(ffq->prod_check + ffq->line_entries) & ffq->entry_mask])
            return -ENOBUFS;
        ffq->prod_check += ffq->line_entries;
    }
    ffq->q[ffq->prod_write & ffq->entry_mask] = (uintptr_t)m;
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
iffq_insert_local(Iffq *ffq, Mbuf *m)
{
    ffq->prod_cache[ffq->prod_cache_write++] = (uintptr_t)m;
    ffq->prod_write++;
}

static inline void
iffq_insert_publish(Iffq *ffq)
{
    unsigned int w = ffq->prod_write_pub;
    for (unsigned int i = 0; i < ffq->prod_cache_write; i++, w++) {
        ffq->q[w & ffq->entry_mask] = ffq->prod_cache[i];
    }
    ffq->prod_write_pub   = w;
    ffq->prod_cache_write = 0;
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
    uintptr_t m = ffq->q[ffq->cons_read & ffq->entry_mask];
    if (m) {
        ffq->cons_read++;
    }
    return (Mbuf *)m;
}

/**
 * iffq_clear - clear the previously extracted entries
 * @ffq: the mailbox to be cleared
 *
 */
static inline void
iffq_clear(Iffq *ffq)
{
    unsigned int s = (ffq->cons_read - ffq->line_entries) & ffq->line_mask;

    for (; (ffq->cons_clear /* & ffq->line_mask */) != s; ffq->cons_clear++) {
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
    const unsigned int pool_mask = g->qlen - 1;
    Iffq *const ffq              = g->ffq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;

    g->producer_header();

    while (!stop) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
#ifdef QDEBUG
        iffq_dump("P", ffq);
#endif
        if (kMbufMode != MbufMode::NoAccess) {
            /* Here we need a StoreStore barrier, to prevent writes to the
             * mbufs to be reordered after the write to the queue slot. */
            compiler_barrier();
        }
        if (iffq_insert(ffq, m) == 0) {
            ++batch_packets;
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            pool_idx--;
        }
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
    Iffq *const ffq            = g->ffq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    Mbuf *m;

    g->consumer_header();

    for (;;) {
#ifdef QDEBUG
        iffq_dump("C", ffq);
#endif
        m = iffq_extract(ffq);
        if (m) {
            ++g->pkt_cnt;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &csum);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_cycles(spin);
            }
            iffq_clear(ffq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(stop)) {
                break;
            }
        }
    }
    g->csum             = csum;
    g->consumer_batches = batches;
    g->consumer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
biffq_producer(Global *const g)
{
    const uint64_t spin          = g->prod_spin_cycles;
    const unsigned int pool_mask = g->qlen - 1;
    const unsigned int batch     = g->prod_batch;
    Iffq *const ffq              = g->ffq;
    unsigned int pool_idx        = 0;
    unsigned int batch_packets   = 0;
    unsigned int batches         = 0;

    g->producer_header();

    while (!stop) {
        unsigned int avail = iffq_wspace(ffq);

#ifdef QDEBUG
        iffq_dump("P", ffq);
#endif

        if (avail) {
            if (avail > batch) {
                avail = batch;
            }
            batch_packets += avail;
            for (; avail > 0; avail--) {
                Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
                iffq_insert_local(ffq, m);
                if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_cycles(spin);
                }
            }
            if (kMbufMode != MbufMode::NoAccess) {
                compiler_barrier();
            }
            iffq_insert_publish(ffq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
    }
    g->producer_batches = batches;
    g->producer_footer();
}

#define biffq_consumer iffq_consumer

/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/*
 * Support for latency tests.
 */
template <MbufMode kMbufMode>
void
lq_client(Global *const g)
{
    const unsigned int pool_mask = g->blq->qmask;
    Blq *const blq               = g->blq;
    Blq *const blq_back          = g->blq_back;
    unsigned int pool_idx        = 0;
    int ret;

    g->producer_header();
    while (!stop) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
        ret     = lq_write(blq, m);
        assert(ret == 0);
        while ((m = lq_read(blq_back)) == nullptr && !stop) {
        }
        ++g->pkt_cnt;
    }
    g->producer_footer();
}

template <MbufMode kMbufMode>
void
lq_server(Global *const g)
{
    Blq *const blq      = g->blq;
    Blq *const blq_back = g->blq_back;
    unsigned int csum   = 0;
    int ret;

    g->consumer_header();
    while (!stop) {
        Mbuf *m;
        while ((m = lq_read(blq)) == nullptr) {
            if (unlikely(stop)) {
                goto out;
            }
        }
        mbuf_put<kMbufMode>(m, &csum);
        ret = lq_write(blq_back, m);
        assert(ret == 0);
    }
out:
    g->csum = csum;
    g->consumer_footer();
}

#define blq_client lq_client
#define blq_server lq_server

template <MbufMode kMbufMode>
void
ffq_client(Global *const g)
{
    const unsigned int pool_mask = g->ffq->entry_mask;
    Iffq *const ffq              = g->ffq;
    Iffq *const ffq_back         = g->ffq_back;
    unsigned int pool_idx        = 0;
    int ret;

    g->producer_header();
    while (!stop) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
        ret     = ffq_write(ffq, m);
        assert(ret == 0);
        while ((m = ffq_read(ffq_back)) == nullptr && !stop) {
        }
        ++g->pkt_cnt;
    }
    g->producer_footer();
}

template <MbufMode kMbufMode>
void
ffq_server(Global *const g)
{
    Iffq *const ffq      = g->ffq;
    Iffq *const ffq_back = g->ffq_back;
    unsigned int csum    = 0;
    int ret;

    g->consumer_header();
    while (!stop) {
        Mbuf *m;
        while ((m = ffq_read(ffq)) == nullptr) {
            if (unlikely(stop)) {
                goto out;
            }
        }
        mbuf_put<kMbufMode>(m, &csum);
        ret = ffq_write(ffq_back, m);
        assert(ret == 0);
    }
out:
    g->csum = csum;
    g->consumer_footer();
}

template <MbufMode kMbufMode>
void
iffq_client(Global *const g)
{
    const unsigned int pool_mask = g->ffq->entry_mask;
    Iffq *const ffq              = g->ffq;
    Iffq *const ffq_back         = g->ffq_back;
    unsigned int pool_idx        = 0;
    int ret;

    g->producer_header();
    while (!stop) {
        Mbuf *m = mbuf_get<kMbufMode>(g, &pool_idx, pool_mask);
        ret     = iffq_insert(ffq, m);
        assert(ret == 0);
        while ((m = iffq_extract(ffq_back)) == nullptr && !stop) {
        }
        iffq_clear(ffq_back);
        ++g->pkt_cnt;
    }
    g->producer_footer();
}

template <MbufMode kMbufMode>
void
iffq_server(Global *const g)
{
    Iffq *const ffq      = g->ffq;
    Iffq *const ffq_back = g->ffq_back;
    unsigned int csum    = 0;
    int ret;

    g->consumer_header();
    while (!stop) {
        Mbuf *m;
        while ((m = iffq_extract(ffq)) == nullptr) {
            if (unlikely(stop)) {
                goto out;
            }
        }
        iffq_clear(ffq);
        mbuf_put<kMbufMode>(m, &csum);
        ret = iffq_insert(ffq_back, m);
        assert(ret == 0);
    }
out:
    g->csum = csum;
    g->consumer_footer();
}

#define biffq_client iffq_client
#define biffq_server iffq_server

    /* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

#if 1
using pc_function_t = void (*)(Global *const);
#else
/* Just using std::function makes the processing loops slower.
 * I cannot believe it! It could be due to some differences in how
 * code is laid out in memory... maybe it has effect on the
 * CPU branch predictor?
 */
using pc_function_t = std::function<void(Global *const)>;
#endif

static void
perf_measure(Global *const g, bool producer)
{
    char filename[32] = "/tmp/spscq-perfXXXXXX";
    std::string cmd;
    int seconds = g->duration - 2;
    int ret, fd;

    if (seconds < 1) {
        printf("[WRN] Not enough time to use perf stat\n");
        return;
    }

    fd = mkstemp(filename);
    if (fd < 0) {
        printf("[ERR] Failed to open temporary file: %s\n", strerror(errno));
        return;
    }
    close(fd);

    {
        std::stringstream ss;
        ss << "./get-perf-top-results.sh " << (producer ? g->p_core : g->c_core)
           << " " << seconds << " " << filename;
        cmd = ss.str();
    }
    /* Wait a bit to make sure producer and consumer have started. */
    usleep(350000);

    printf("Running command '%s'\n", cmd.c_str());
    ret = system(cmd.c_str());
    if (ret) {
        printf("[ERR] Command '%s' failed\n", cmd.c_str());
        return;
    }

    std::ifstream fin(filename);
    float &miss_rate = producer ? g->prod_miss_rate : g->cons_miss_rate;
    float &insn_rate = producer ? g->prod_insn_rate : g->cons_insn_rate;
    miss_rate        = 0.0;
    insn_rate        = 0.0;
    fin >> miss_rate >> insn_rate;
    fin.close();
    remove(filename);
}

static void
control_thread(Global *const g)
{
    auto t_last                         = std::chrono::system_clock::now();
    auto t_first                        = t_last;
    long long unsigned int pkt_cnt_last = 0;

    for (unsigned int loopcnt = 1; !stop; loopcnt++) {
        usleep(250000);

        if (loopcnt % 8 == 0) {
            auto t_now = std::chrono::system_clock::now();
            long long unsigned int pkt_cnt_now = g->pkt_cnt;
            double mpps = (pkt_cnt_now - pkt_cnt_last) * 1000.0 /
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              t_now - t_last)
                              .count();

            if (g->online_rate) {
                printf("%3.3f Mpps\n", mpps);
            }

            if (g->num_packets > 0 && pkt_cnt_now > g->num_packets) {
                /* Packet threshold reached. */
                stop = 1;
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(t_now -
                                                                 t_first)
                    .count() >= g->duration) {
                /* We ran out of time. */
                stop = 1;
            }

            t_last       = t_now;
            pkt_cnt_last = pkt_cnt_now;
        }
    }
}

static int
run_test(Global *g)
{
    std::map<
        std::string,
        std::map<MbufMode,
                 std::map<RateLimitMode,
                          std::map<EmulatedOverhead,
                                   std::pair<pc_function_t, pc_function_t>>>>>
        throughput_matrix;
    std::map<std::string,
             std::map<MbufMode, std::pair<pc_function_t, pc_function_t>>>
        latency_matrix;
    std::pair<pc_function_t, pc_function_t> funcs;
    std::thread pth;
    std::thread cth;
    std::thread perf_th_p, perf_th_c;
    std::thread ctrl_th;
    bool use_perf_tool = g->perf_counters && g->p_core != -1 &&
                         g->c_core != -1 && g->p_core != g->c_core;

#define __STRFY(x) #x
#define STRFY(x) __STRFY(x)

#define __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl, eo)                       \
    throughput_matrix[STRFY(qname)][mm][rl][eo] = std::make_pair(              \
        qname##_producer<mm, rl, eo>, qname##_consumer<mm, rl, eo>)

#define __MATRIX_ADD_RATELIMITMODE(qname, mm, rl)                              \
    do {                                                                       \
        __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl, EmulatedOverhead::None);  \
        __MATRIX_ADD_EMULATEDOVERHEAD(qname, mm, rl,                           \
                                      EmulatedOverhead::SpinCycles);           \
    } while (0)
#define __MATRIX_ADD_MBUFMODE(qname, mm)                                       \
    do {                                                                       \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::None);            \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::Limit);           \
        latency_matrix[STRFY(qname)][mm] =                                     \
            std::make_pair(qname##_client<mm>, qname##_server<mm>);            \
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
    /* Improved fast-forward queue with batching capabilities. */
    MATRIX_ADD(biffq);

    if (throughput_matrix.count(g->test_type) == 0) {
        printf("Error: unknown test type '%s'\n", g->test_type.c_str());
        exit(EXIT_FAILURE);
    }
    RateLimitMode rl = g->cons_rate_limit_cycles > 0 ? RateLimitMode::Limit
                                                     : RateLimitMode::None;
    EmulatedOverhead eo = (g->prod_spin_cycles == 0 && g->cons_spin_cycles == 0)
                              ? EmulatedOverhead::None
                              : EmulatedOverhead::SpinCycles;
    funcs = g->latency ? latency_matrix[g->test_type][g->mbuf_mode]
                       : throughput_matrix[g->test_type][g->mbuf_mode][rl][eo];

    if (g->test_type == "lq" || g->test_type == "blq") {
        g->blq = blq_create(g->qlen, g->prod_batch, g->cons_batch);
        if (!g->blq) {
            exit(EXIT_FAILURE);
        }
        blq_dump("P", g->blq);
        if (g->latency) {
            g->blq_back = blq_create(g->qlen, g->prod_batch, g->cons_batch);
            if (!g->blq_back) {
                exit(EXIT_FAILURE);
            }
        }
    } else if (g->test_type == "ffq") {
        g->ffq = ffq_create(g->qlen,
                            /*line_size=*/g->line_entries * sizeof(uintptr_t));
        if (!g->ffq) {
            exit(EXIT_FAILURE);
        }
        iffq_dump("P", g->ffq);
        if (g->latency) {
            g->ffq_back =
                ffq_create(g->qlen,
                           /*line_size=*/g->line_entries * sizeof(uintptr_t));
            if (!g->ffq_back) {
                exit(EXIT_FAILURE);
            }
        }
    } else if (g->test_type == "iffq" || g->test_type == "biffq") {
        (void)iffq_prefetch;
        g->ffq = iffq_create(g->qlen,
                             /*line_size=*/g->line_entries * sizeof(uintptr_t));
        if (!g->ffq) {
            exit(EXIT_FAILURE);
        }
        iffq_dump("P", g->ffq);
        if (g->latency) {
            g->ffq_back =
                iffq_create(g->qlen,
                            /*line_size=*/g->line_entries * sizeof(uintptr_t));
            if (!g->ffq_back) {
                exit(EXIT_FAILURE);
            }
        }
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
    if (use_perf_tool) {
        perf_th_p = std::thread(perf_measure, g, /*producer=*/true);
        perf_th_c = std::thread(perf_measure, g, /*producer=*/false);
    }
    ctrl_th = std::thread(control_thread, g);
    pth.join();
    cth.join();
    ctrl_th.join();
    if (use_perf_tool) {
        perf_th_p.join();
        perf_th_c.join();
    }

    g->print_results();

    free(g->pool);
    if (g->blq) {
        blq_free(g->blq);
        g->blq = nullptr;
    }
    if (g->blq_back) {
        blq_free(g->blq_back);
        g->blq_back = nullptr;
    }
    if (g->ffq) {
        iffq_free(g->ffq);
        g->ffq = nullptr;
    }
    if (g->ffq_back) {
        iffq_free(g->ffq_back);
        g->ffq_back = nullptr;
    }

    return 0;
}

static void
usage(const char *progname)
{
    printf("%s [-h]\n"
           "    [-n NUM_PACKETS (in millions) = inf]\n"
           "    [-D DURATION (in seconds) = %d]\n"
           "    [-b MAX_PRODUCER_BATCH = %d]\n"
           "    [-b MAX_CONSUMER_BATCH = %d]\n"
           "    [-l QUEUE_LENGTH = %d]\n"
           "    [-L LINE_ENTRIES (iffq) = %d]\n"
           "    [-c PRODUCER_CORE_ID = 0]\n"
           "    [-c CONSUMER_CORE_ID = 1]\n"
           "    [-P PRODUCER_SPIN (cycles, ns) = 0]\n"
           "    [-C CONSUMER_SPIN (cycles, ns) = 0]\n"
           "    [-t TEST_TYPE (lq,blq,ffq,iffq,biffq)]\n"
           "    [-M (access mbuf content)]\n"
           "    [-r CONSUMER_RATE_LIMIT_NS = 0]\n"
           "    [-R (use online rating)]\n"
           "    [-p (use CPU performance counters)]\n"
           "    [-T (carry out latency tests rather than throughput tests)]\n"
           "\n",
           progname, Global::DFLT_D, Global::DFLT_BATCH, Global::DFLT_BATCH,
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

    while ((opt = getopt(argc, argv, "hn:b:l:c:t:L:P:C:Mr:RpD:T")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            g->num_packets = atoi(optarg) * 1000000ULL;
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
            g->prod_spin_cycles = atoi(optarg);
            if (g->prod_spin_cycles < 0) {
                printf("    Invalid producer spin '%s'\n", optarg);
                return -1;
            }
            break;

        case 'C':
            g->cons_spin_cycles = atoi(optarg);
            if (g->cons_spin_cycles < 0) {
                printf("    Invalid consumer spin '%s'\n", optarg);
                return -1;
            }
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
            g->cons_rate_limit_cycles = atoi(optarg);
            if (g->cons_rate_limit_cycles < 0) {
                printf("    Invalid consumer rate limit '%s'\n", optarg);
                return -1;
            }
            break;

        case 'R':
            g->online_rate = true;
            break;

        case 'p':
            g->perf_counters = true;
            break;

        case 'D':
            g->duration = atoi(optarg);
            if (g->duration < 1) {
                printf("    Invalid test duration '%s' (in seconds)\n", optarg);
                return -1;
            }
            break;

        case 'T':
            g->latency = true;
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
