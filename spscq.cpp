/*
 * 2017-2018 Vincenzo Maffione (Universita' di Pisa)
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
#include <sys/mman.h>

#include "mlib.h"

/* This must be defined before including spscq.h. */
static unsigned short *smap = nullptr;
#if 1
#define SMAP(x) smap[x]
#else
#define SMAP(x) x
#endif

#include "spscq.h"

//#define RATE_LIMITING_CONSUMER /* Enable support for rate limiting consumer */

#undef QDEBUG /* dump queue state at each operation */

#define ONEBILLION (1000LL * 1000000LL) /* 1 billion */

static int stop = 0;

static void
sigint_handler(int signum)
{
    ACCESS_ONCE(stop) = 1;
}

/* Alloc zeroed cacheline-aligned memory, aborting on failure. */
static void *
szalloc(size_t size, bool hugepages)
{
    void *p = NULL;
    if (hugepages) {
        p = mmap(NULL, size, PROT_WRITE | PROT_READ,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (p == MAP_FAILED) {
            printf("mmap allocation failure: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        assert(reinterpret_cast<uint64_t>(p) % ALIGN_SIZE == 0);
    } else {
        int ret = posix_memalign(&p, ALIGN_SIZE, size);
        if (ret) {
            printf("allocation failure: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        memset(p, 0, size);
    }
    return p;
}

static void
sfree(void *ptr, size_t size, bool hugepages)
{
    if (hugepages) {
        munmap(ptr, size);
    } else {
        free(ptr);
    }
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
};

struct Mbuf {
    unsigned int len;
    unsigned int __padding[7];
#define MBUF_LEN_MAX (4096 + CACHELINE_SIZE - 8 * sizeof(unsigned int))
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
     * in TSC ticks (initially in nanoseconds). */
    uint64_t prod_spin_ticks = 0, cons_spin_ticks = 0;
    uint64_t cons_rate_limit_cycles = 0;

    bool online_rate   = false;
    bool perf_counters = false;

    /* Test duration in seconds. */
    unsigned int duration = DFLT_D;

    /* Type of queue used. */
    std::string test_type = "lq";

    /* If true we do a latency test; if false we do a throughput test. */
    bool latency = false;

    /* Try to keep hardware prefetcher disabled for the accesses to the
     * queue slots. This should reduce the noise in the cache miss
     * behaviour. */
    bool deceive_hw_data_prefetcher = false;

    /* Allocate memory from hugepages. */
    bool hugepages = false;

    MbufMode mbuf_mode = MbufMode::NoAccess;

    /* Timestamp to compute experiment statistics. */
    std::chrono::system_clock::time_point begin, end;

    /* Checksum for when -M is used. */
    unsigned int csum;

    /* When -M is used, we need a variable to increment that depends on
     * something contained inside the mbuf; this is needed to make sure
     * that spin_for() has a data dependency on the mbuf_get() or
     * mbuf_put(), so that spin_for() does not run in the parallel
     * with mbuf_put() or mbuf_get().
     * To avoid the compiler optimizing out this variable, P and C
     * save it to the 'trash' global variable here. */
    unsigned int trash;

    /* Packet count written back by consumers. It's safer for it
     * to have its own cacheline. */
    CACHELINE_ALIGNED
    volatile long long unsigned pkt_cnt = 0;

    /* Average batches as seen by producer and consumer. */
    CACHELINE_ALIGNED
    long long int producer_batches = 0;
    long long int consumer_batches = 0;

    /* L1 dcache miss rates in M/sec. */
    float prod_read_miss_rate  = 0.0;
    float cons_read_miss_rate  = 0.0;
    float prod_write_miss_rate = 0.0;
    float cons_write_miss_rate = 0.0;

    /* CPU instruction rate in B/sec. */
    float prod_insn_rate = 0.0;
    float cons_insn_rate = 0.0;

    /* The lamport-like queue. */
    Blq *blq      = nullptr;
    Blq *blq_back = nullptr;

    /* The ff-like queue. */
    Iffq *ffq      = nullptr;
    Iffq *ffq_back = nullptr;

    /* A pool of preallocated mbufs (only accessed by P). */
    Mbuf *pool = nullptr;

    /* Index in the mbuf pool array (only accessed by P). */
    unsigned int pool_idx = 0;

    /* Maks for the mbuf pool array (only accessed by P). */
    unsigned int pool_mask;

    void producer_header();
    void producer_footer();
    void consumer_header();
    void consumer_footer();
    void print_results();
};

static void
miss_rate_print(const char *prefix, double mpps, float read_miss_rate,
                float write_miss_rate)
{
    printf("[%s] L1 d-cache %.3f rmisses/packet,"
           "%.3f wmisses/packet\n",
           prefix, read_miss_rate / mpps, write_miss_rate / mpps);
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
    miss_rate_print("P", mpps, prod_read_miss_rate, prod_write_miss_rate);
    miss_rate_print("C", mpps, cons_read_miss_rate, cons_write_miss_rate);
    printf("%3.3f Mpps %2.3f Pmpp %2.3f Cmpp\n", mpps,
           (prod_read_miss_rate + prod_write_miss_rate) / mpps,
           (cons_read_miss_rate + cons_write_miss_rate) / mpps);
}

void
Global::producer_header()
{
    runon("P", p_core);
    begin    = std::chrono::system_clock::now();
    pool_idx = 0;
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
mbuf_get(Global *const g, unsigned int trash)
{
    if (kMbufMode == MbufMode::NoAccess) {
        return &gm;
    } else {
        Mbuf *m = &g->pool[g->pool_idx & g->pool_mask];
        /* We want that m->len depends on trash but we
         * don't want to put trash inside m->len (to
         * preserve the checksum). */
        m->len = g->pool_idx++ + !!(trash == 0xdeadbeef);
        return m;
    }
}

template <MbufMode kMbufMode>
static inline void
mbuf_put(Mbuf *const m, unsigned int *csum, unsigned int *trash)
{
    if (kMbufMode != MbufMode::NoAccess) {
        *csum += m->len;
        *trash += *csum;
    }
}

static void
qslotmap_init(unsigned short *qslotmap, unsigned qlen, bool shuffle)
{
    if (!shuffle) {
        for (unsigned i = 0; i < qlen; i++) {
            qslotmap[i] = i;
        }
        return;
    }
    /* Prepare support for shuffled queue slots to disable the effect of hw
     * prefetching on the queue slots.
     * K is the number of entries per cacheline. */
    unsigned int K = CACHELINE_SIZE / sizeof(uintptr_t);
    std::vector<unsigned short> v(qlen / K);
    std::random_device rd;
    std::mt19937 gen(rd());

    /* First create a vector of qlen/K elements (i.e. the number of cache
     * lines in the queue), containing a random permutation of
     * K*[0..qlen/K[. */
    for (size_t i = 0; i < v.size(); i++) {
        v[i] = K * i;
    }
    std::shuffle(v.begin(), v.end(), gen);

    for (unsigned j = 0; j < qlen / K; j++) {
        std::vector<unsigned short> u(K);

        /* Generate slot indices for the #j cacheline, as a random
         * permutation of [v[j]..v[j]+K[ */
        for (size_t i = 0; i < K; i++) {
            u[i] = v[j] + i;
        }
        std::shuffle(u.begin(), u.end(), gen);
        for (size_t i = 0; i < K; i++) {
            qslotmap[j * K + i] = u[i];
        }
    }
}

static Blq *
blq_create(int qlen, bool hugepages)
{
    Blq *blq = static_cast<Blq *>(szalloc(blq_size(qlen), hugepages));
    int ret;

    ret = blq_init(blq, qlen);
    if (ret) {
        return NULL;
    }

    assert(reinterpret_cast<uintptr_t>(blq) % ALIGN_SIZE == 0);
    assert((reinterpret_cast<uintptr_t>(&blq->write)) -
               (reinterpret_cast<uintptr_t>(&blq->write_priv)) ==
           ALIGN_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->read_priv)) -
               (reinterpret_cast<uintptr_t>(&blq->write)) ==
           ALIGN_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->read)) -
               (reinterpret_cast<uintptr_t>(&blq->read_priv)) ==
           ALIGN_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->qlen)) -
               (reinterpret_cast<uintptr_t>(&blq->read)) ==
           ALIGN_SIZE);
    assert((reinterpret_cast<uintptr_t>(&blq->q[0])) -
               (reinterpret_cast<uintptr_t>(&blq->qlen)) ==
           ALIGN_SIZE);

    return blq;
}

static void
blq_free(Blq *blq, bool hugepages)
{
    memset(blq, 0, sizeof(*blq));
    sfree(blq, blq_size(blq->qlen), hugepages);
}

template <MbufMode kMbufMode>
static inline void
spin_for(uint64_t spin, unsigned int *trash)
{
    uint64_t when = rdtsc() + spin;

    while (rdtsc() < when) {
        if (kMbufMode != MbufMode::NoAccess) {
            (*trash)++;
        }
        compiler_barrier();
    }
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
lq_producer(Global *const g)
{
    const uint64_t spin        = g->prod_spin_ticks;
    Blq *const blq             = g->blq;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;

    g->producer_header();
    while (!ACCESS_ONCE(stop)) {
        if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
            spin_for<kMbufMode>(spin, &trash);
        }

        Mbuf *m = mbuf_get<kMbufMode>(g, trash);
#ifdef QDEBUG
        blq_dump("P", blq);
#endif
        while (lq_write(blq, (uintptr_t)m)) {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
        ++batch_packets;
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
lq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_ticks;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    Blq *const blq             = g->blq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;
    Mbuf *m;

    g->consumer_header();
    for (;;) {
#ifdef QDEBUG
        blq_dump("C", blq);
#endif
        m = (Mbuf *)lq_read(blq);
        if (m) {
            ++g->pkt_cnt;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &csum, &trash);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_for<kMbufMode>(spin, &trash);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(ACCESS_ONCE(stop))) {
                break;
            }
        }
    }
    g->consumer_batches = batches;
    g->csum             = csum;
    g->trash            = trash;
    g->consumer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
llq_producer(Global *const g)
{
    const uint64_t spin        = g->prod_spin_ticks;
    Blq *const blq             = g->blq;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;

    g->producer_header();
    while (!ACCESS_ONCE(stop)) {
        if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
            spin_for<kMbufMode>(spin, &trash);
        }

        Mbuf *m = mbuf_get<kMbufMode>(g, trash);
#ifdef QDEBUG
        blq_dump("P", blq);
#endif
        while (llq_write(blq, (uintptr_t)m)) {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
        ++batch_packets;
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
llq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_ticks;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    Blq *const blq             = g->blq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;
    Mbuf *m;

    g->consumer_header();
    for (;;) {
#ifdef QDEBUG
        blq_dump("C", blq);
#endif
        m = (Mbuf *)llq_read(blq);
        if (m) {
            ++g->pkt_cnt;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &csum, &trash);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_for<kMbufMode>(spin, &trash);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(ACCESS_ONCE(stop))) {
                break;
            }
        }
    }
    g->consumer_batches = batches;
    g->csum             = csum;
    g->trash            = trash;
    g->consumer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
blq_producer(Global *const g)
{
    const uint64_t spin        = g->prod_spin_ticks;
    const unsigned int batch   = g->prod_batch;
    Blq *const blq             = g->blq;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;

    g->producer_header();

    while (!ACCESS_ONCE(stop)) {
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
                if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_for<kMbufMode>(spin, &trash);
                }
                Mbuf *m = mbuf_get<kMbufMode>(g, trash);
                blq_write_local(blq, (uintptr_t)m);
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
    const uint64_t spin        = g->cons_spin_ticks;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    const unsigned int batch   = g->cons_batch;
    Blq *const blq             = g->blq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;
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
                m = (Mbuf *)blq_read_local(blq);
                mbuf_put<kMbufMode>(m, &csum, &trash);
                if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_for<kMbufMode>(spin, &trash);
                }
            }
            blq_read_publish(blq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(ACCESS_ONCE(stop))) {
                break;
            }
        }
    }
    g->csum             = csum;
    g->trash            = trash;
    g->consumer_batches = batches;
    g->consumer_footer();
}
/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
ffq_producer(Global *const g)
{
    const uint64_t spin        = g->prod_spin_ticks;
    Iffq *const ffq            = g->ffq;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;

    g->producer_header();

    while (!ACCESS_ONCE(stop)) {
        if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
            spin_for<kMbufMode>(spin, &trash);
        }

        Mbuf *m = mbuf_get<kMbufMode>(g, trash);

        if (kMbufMode != MbufMode::NoAccess) {
            compiler_barrier();
        }
        while (ffq_write(ffq, (uintptr_t)m)) {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
        ++batch_packets;
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
ffq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_ticks;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    Iffq *const ffq            = g->ffq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;
    Mbuf *m;

    g->consumer_header();

    for (;;) {
        m = (Mbuf *)ffq_read(ffq);
        if (m) {
            ++batch_packets;
            ++g->pkt_cnt;
            mbuf_put<kMbufMode>(m, &csum, &trash);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_for<kMbufMode>(spin, &trash);
            }
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(ACCESS_ONCE(stop))) {
                break;
            }
        }
    }
    g->csum             = csum;
    g->trash            = trash;
    g->consumer_batches = batches;
    g->consumer_footer();
}

/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

/**
 * iffq_create - create a new mailbox
 * @entries: the number of entries
 * @line_size: the line size in bytes (not in entries)
 *
 * Both entries and line_size must be a power of 2.
 */
Iffq *
__iffq_create(unsigned int entries, unsigned int line_size, bool hugepages,
              bool improved)
{
    Iffq *ffq;
    int err;

    ffq = static_cast<Iffq *>(szalloc(iffq_size(entries), hugepages));

    err = iffq_init(ffq, entries, line_size, improved);
    if (err) {
        free(ffq);
        return NULL;
    }

    assert(reinterpret_cast<uintptr_t>(ffq) % ALIGN_SIZE == 0);
    assert(((reinterpret_cast<uintptr_t>(&ffq->cons_clear)) -
            (reinterpret_cast<uintptr_t>(&ffq->prod_write))) %
               ALIGN_SIZE ==
           0);
    assert((reinterpret_cast<uintptr_t>(&ffq->q[0])) -
               (reinterpret_cast<uintptr_t>(&ffq->cons_clear)) ==
           ALIGN_SIZE);

    return ffq;
}

Iffq *
iffq_create(unsigned int entries, unsigned int line_size, bool hugepages)
{
    return __iffq_create(entries, line_size, hugepages, /*improved=*/true);
}

Iffq *
ffq_create(unsigned int entries, unsigned int line_size, bool hugepages)
{
    return __iffq_create(entries, line_size, hugepages, /*improved=*/false);
}

/**
 * iffq_free - delete a mailbox
 * @ffq: the mailbox to be deleted
 */
void
iffq_free(Iffq *ffq, bool hugepages)
{
    sfree(ffq, iffq_size(ffq->entry_mask + 1), hugepages);
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
iffq_producer(Global *const g)
{
    const uint64_t spin        = g->prod_spin_ticks;
    struct Iffq *const ffq     = g->ffq;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;

    g->producer_header();

    while (!ACCESS_ONCE(stop)) {
        if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
            spin_for<kMbufMode>(spin, &trash);
        }

        Mbuf *m = mbuf_get<kMbufMode>(g, trash);
#ifdef QDEBUG
        iffq_dump("P", ffq);
#endif
        if (kMbufMode != MbufMode::NoAccess) {
            /* Here we need a StoreStore barrier, to prevent writes to the
             * mbufs to be reordered after the write to the queue slot. */
            compiler_barrier();
        }
        while (iffq_insert(ffq, (uintptr_t)m)) {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
        }
        ++batch_packets;
    }
    g->producer_batches = batches;
    g->producer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
iffq_consumer(Global *const g)
{
    const uint64_t spin        = g->cons_spin_ticks;
    const uint64_t rate_limit  = g->cons_rate_limit_cycles;
    Iffq *const ffq            = g->ffq;
    unsigned int csum          = 0;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;
    Mbuf *m;

    g->consumer_header();

    for (;;) {
#ifdef QDEBUG
        iffq_dump("C", ffq);
#endif
        m = (Mbuf *)iffq_extract(ffq);
        if (m) {
            ++g->pkt_cnt;
            ++batch_packets;
            mbuf_put<kMbufMode>(m, &csum, &trash);
            if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                spin_for<kMbufMode>(spin, &trash);
            }
            iffq_clear(ffq);
        } else {
            batches += (batch_packets != 0) ? 1 : 0;
            batch_packets = 0;
            if (kRateLimitMode == RateLimitMode::Limit) {
                tsc_sleep_till(rdtsc() + rate_limit);
            }
            if (unlikely(ACCESS_ONCE(stop))) {
                break;
            }
        }
    }
    g->csum             = csum;
    g->trash            = trash;
    g->consumer_batches = batches;
    g->consumer_footer();
}

template <MbufMode kMbufMode, RateLimitMode kRateLimitMode,
          EmulatedOverhead kEmulatedOverhead>
static void
biffq_producer(Global *const g)
{
    const uint64_t spin        = g->prod_spin_ticks;
    const unsigned int batch   = g->prod_batch;
    Iffq *const ffq            = g->ffq;
    unsigned int batch_packets = 0;
    unsigned int batches       = 0;
    unsigned int trash         = 0;

    g->producer_header();

    while (!ACCESS_ONCE(stop)) {
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
                if (kEmulatedOverhead == EmulatedOverhead::SpinCycles) {
                    spin_for<kMbufMode>(spin, &trash);
                }
                Mbuf *m = mbuf_get<kMbufMode>(g, trash);
                iffq_insert_local(ffq, (uintptr_t)m);
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
    Blq *const blq      = g->blq;
    Blq *const blq_back = g->blq_back;
    int ret;

    g->producer_header();
    while (!ACCESS_ONCE(stop)) {
        Mbuf *m = mbuf_get<kMbufMode>(g, 0);
        ret     = lq_write(blq, (uintptr_t)m);
        assert(ret == 0);
        while ((m = (Mbuf *)lq_read(blq_back)) == nullptr &&
               !ACCESS_ONCE(stop)) {
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
    unsigned int trash  = 0;
    int ret;

    g->consumer_header();
    for (;;) {
        Mbuf *m;
        while ((m = (Mbuf *)lq_read(blq)) == nullptr) {
            if (unlikely(ACCESS_ONCE(stop))) {
                goto out;
            }
        }
        mbuf_put<kMbufMode>(m, &csum, &trash);
        ret = lq_write(blq_back, (uintptr_t)m);
        assert(ret == 0);
    }
out:
    g->csum  = csum;
    g->trash = trash;
    g->consumer_footer();
}

template <MbufMode kMbufMode>
void
llq_client(Global *const g)
{
    Blq *const blq      = g->blq;
    Blq *const blq_back = g->blq_back;
    int ret;

    g->producer_header();
    while (!ACCESS_ONCE(stop)) {
        Mbuf *m = mbuf_get<kMbufMode>(g, 0);
        ret     = llq_write(blq, (uintptr_t)m);
        assert(ret == 0);
        while ((m = (Mbuf *)llq_read(blq_back)) == nullptr &&
               !ACCESS_ONCE(stop)) {
        }
        ++g->pkt_cnt;
    }
    g->producer_footer();
}

template <MbufMode kMbufMode>
void
llq_server(Global *const g)
{
    Blq *const blq      = g->blq;
    Blq *const blq_back = g->blq_back;
    unsigned int csum   = 0;
    unsigned int trash  = 0;
    int ret;

    g->consumer_header();
    for (;;) {
        Mbuf *m;
        while ((m = (Mbuf *)llq_read(blq)) == nullptr) {
            if (unlikely(ACCESS_ONCE(stop))) {
                goto out;
            }
        }
        mbuf_put<kMbufMode>(m, &csum, &trash);
        ret = llq_write(blq_back, (uintptr_t)m);
        assert(ret == 0);
    }
out:
    g->csum  = csum;
    g->trash = trash;
    g->consumer_footer();
}
#define blq_client llq_client
#define blq_server llq_server

template <MbufMode kMbufMode>
void
ffq_client(Global *const g)
{
    Iffq *const ffq      = g->ffq;
    Iffq *const ffq_back = g->ffq_back;
    int ret;

    g->producer_header();
    while (!ACCESS_ONCE(stop)) {
        Mbuf *m = mbuf_get<kMbufMode>(g, 0);
        ret     = ffq_write(ffq, (uintptr_t)m);
        assert(ret == 0);
        while ((m = (Mbuf *)ffq_read(ffq_back)) == nullptr &&
               !ACCESS_ONCE(stop)) {
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
    unsigned int trash   = 0;
    int ret;

    g->consumer_header();
    for (;;) {
        Mbuf *m;
        while ((m = (Mbuf *)ffq_read(ffq)) == nullptr) {
            if (unlikely(ACCESS_ONCE(stop))) {
                goto out;
            }
        }
        mbuf_put<kMbufMode>(m, &csum, &trash);
        ret = ffq_write(ffq_back, (uintptr_t)m);
        assert(ret == 0);
    }
out:
    g->csum  = csum;
    g->trash = trash;
    g->consumer_footer();
}

template <MbufMode kMbufMode>
void
iffq_client(Global *const g)
{
    Iffq *const ffq      = g->ffq;
    Iffq *const ffq_back = g->ffq_back;
    int ret;

    g->producer_header();
    while (!ACCESS_ONCE(stop)) {
        Mbuf *m = mbuf_get<kMbufMode>(g, 0);
        ret     = iffq_insert(ffq, (uintptr_t)m);
        assert(ret == 0);
        while ((m = (Mbuf *)iffq_extract(ffq_back)) == nullptr &&
               !ACCESS_ONCE(stop)) {
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
    unsigned int trash   = 0;
    int ret;

    g->consumer_header();
    for (;;) {
        Mbuf *m;
        while ((m = (Mbuf *)iffq_extract(ffq)) == nullptr) {
            if (unlikely(ACCESS_ONCE(stop))) {
                goto out;
            }
        }
        iffq_clear(ffq);
        mbuf_put<kMbufMode>(m, &csum, &trash);
        ret = iffq_insert(ffq_back, (uintptr_t)m);
        assert(ret == 0);
    }
out:
    g->csum  = csum;
    g->trash = trash;
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
    float &read_miss_rate =
        producer ? g->prod_read_miss_rate : g->cons_read_miss_rate;
    float &write_miss_rate =
        producer ? g->prod_write_miss_rate : g->cons_write_miss_rate;
    float &insn_rate = producer ? g->prod_insn_rate : g->cons_insn_rate;
    read_miss_rate   = 0.0;
    write_miss_rate  = 0.0;
    insn_rate        = 0.0;
    /* Miss rates are in M/sec, instruction rate is in B/sec. */
    fin >> read_miss_rate >> write_miss_rate >> insn_rate;
    fin.close();
    remove(filename);
}

static void
control_thread(Global *const g)
{
    auto t_last                         = std::chrono::system_clock::now();
    auto t_first                        = t_last;
    long long unsigned int pkt_cnt_last = 0;

    for (unsigned int loopcnt = 1; !ACCESS_ONCE(stop); loopcnt++) {
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
                ACCESS_ONCE(stop) = 1;
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(t_now -
                                                                 t_first)
                    .count() >= g->duration) {
                /* We ran out of time. */
                ACCESS_ONCE(stop) = 1;
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
#ifdef RATE_LIMITING_CONSUMER
#define __MATRIX_ADD_MBUFMODE(qname, mm)                                       \
    do {                                                                       \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::None);            \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::Limit);           \
        latency_matrix[STRFY(qname)][mm] =                                     \
            std::make_pair(qname##_client<mm>, qname##_server<mm>);            \
    } while (0)
#else /* RATE_LIMITING_CONSUMER */
#define __MATRIX_ADD_MBUFMODE(qname, mm)                                       \
    do {                                                                       \
        __MATRIX_ADD_RATELIMITMODE(qname, mm, RateLimitMode::None);            \
        latency_matrix[STRFY(qname)][mm] =                                     \
            std::make_pair(qname##_client<mm>, qname##_server<mm>);            \
    } while (0)
#endif /* RATE_LIMITING_CONSUMER */
#define MATRIX_ADD(qname)                                                      \
    do {                                                                       \
        __MATRIX_ADD_MBUFMODE(qname, MbufMode::NoAccess);                      \
        __MATRIX_ADD_MBUFMODE(qname, MbufMode::LinearAccess);                  \
    } while (0)

    /* Multi-section queue (Lamport-like) with legacy operation,
     * i.e. no batching. */
    MATRIX_ADD(lq);
    MATRIX_ADD(llq);
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

    size_t pool_size          = ALIGNED_SIZE(2 * g->qlen * sizeof(g->pool[0]));
    size_t pool_and_smap_size = pool_size + (g->qlen * sizeof(SMAP(0)));
    /* Allocate mbuf pool and smap together. */
    void *pool_and_smap = szalloc(pool_and_smap_size, g->hugepages);

    /* Init the mbuf pool and the mask. */
    g->pool      = static_cast<Mbuf *>(pool_and_smap);
    g->pool_mask = (2 * g->qlen) - 1;

    /* Init the smap. */
    smap = reinterpret_cast<unsigned short *>(
        (static_cast<char *>(pool_and_smap) + pool_size));
    qslotmap_init(smap, g->qlen, g->deceive_hw_data_prefetcher);
#if 0
    for (unsigned i = 0; i < g->qlen; i++) {
        printf("%d ", SMAP(i));
    }
    printf("\n");
#endif

    RateLimitMode rl = g->cons_rate_limit_cycles > 0 ? RateLimitMode::Limit
                                                     : RateLimitMode::None;
    EmulatedOverhead eo = (g->prod_spin_ticks == 0 && g->cons_spin_ticks == 0)
                              ? EmulatedOverhead::None
                              : EmulatedOverhead::SpinCycles;
    funcs = g->latency ? latency_matrix[g->test_type][g->mbuf_mode]
                       : throughput_matrix[g->test_type][g->mbuf_mode][rl][eo];

    if (g->test_type == "lq" || g->test_type == "llq" ||
        g->test_type == "blq") {
        g->blq = blq_create(g->qlen, g->hugepages);
        if (!g->blq) {
            exit(EXIT_FAILURE);
        }
        blq_dump("P", g->blq);
        if (g->latency) {
            g->blq_back = blq_create(g->qlen, g->hugepages);
            if (!g->blq_back) {
                exit(EXIT_FAILURE);
            }
        }
    } else if (g->test_type == "ffq") {
        g->ffq = ffq_create(g->qlen,
                            /*line_size=*/g->line_entries * sizeof(uintptr_t),
                            g->hugepages);
        if (!g->ffq) {
            exit(EXIT_FAILURE);
        }
        iffq_dump("P", g->ffq);
        if (g->latency) {
            g->ffq_back =
                ffq_create(g->qlen,
                           /*line_size=*/g->line_entries * sizeof(uintptr_t),
                           g->hugepages);
            if (!g->ffq_back) {
                exit(EXIT_FAILURE);
            }
        }
    } else if (g->test_type == "iffq" || g->test_type == "biffq") {
        (void)iffq_prefetch;
        g->ffq = iffq_create(g->qlen,
                             /*line_size=*/g->line_entries * sizeof(uintptr_t),
                             g->hugepages);
        if (!g->ffq) {
            exit(EXIT_FAILURE);
        }
        iffq_dump("P", g->ffq);
        if (g->latency) {
            g->ffq_back =
                iffq_create(g->qlen,
                            /*line_size=*/g->line_entries * sizeof(uintptr_t),
                            g->hugepages);
            if (!g->ffq_back) {
                exit(EXIT_FAILURE);
            }
        }
    } else {
        assert(0);
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

    sfree(pool_and_smap, pool_and_smap_size, g->hugepages);
    if (g->blq) {
        blq_free(g->blq, g->hugepages);
        g->blq = nullptr;
    }
    if (g->blq_back) {
        blq_free(g->blq_back, g->hugepages);
        g->blq_back = nullptr;
    }
    if (g->ffq) {
        iffq_free(g->ffq, g->hugepages);
        g->ffq = nullptr;
    }
    if (g->ffq_back) {
        iffq_free(g->ffq_back, g->hugepages);
        g->ffq_back = nullptr;
    }

    return 0;
}

static void
usage(const char *progname)
{
    printf(
        "%s\n"
        "    [-h (show this help and exit)]\n"
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
        "    [-w (try to prevent the hw prefetcher to prefetch queue slots)]\n"
        "    [-H (allocate memory from hugepages)]\n"
        "\n",
        progname, Global::DFLT_D, Global::DFLT_BATCH, Global::DFLT_BATCH,
        Global::DFLT_QLEN, Global::DFLT_LINE_ENTRIES);
}

int
main(int argc, char **argv)
{
    bool got_b_option = false;
    bool got_c_option = false;
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

    while ((opt = getopt(argc, argv, "hn:b:l:c:t:L:P:C:Mr:RpD:TwH")) != -1) {
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
            if (val >= 0) {
                if (got_c_option) {
                    g->c_core = val;
                } else {
                    g->p_core    = val;
                    got_c_option = true;
                }
            }
            break;
        }

        case 't':
            g->test_type = std::string(optarg);
            break;

        case 'P':
            g->prod_spin_ticks = atoi(optarg);
            if (g->prod_spin_ticks < 0) {
                printf("    Invalid producer spin '%s'\n", optarg);
                return -1;
            }
            break;

        case 'C':
            g->cons_spin_ticks = atoi(optarg);
            if (g->cons_spin_ticks < 0) {
                printf("    Invalid consumer spin '%s'\n", optarg);
                return -1;
            }
            break;

        case 'M':
            switch (g->mbuf_mode) {
            case MbufMode::NoAccess:
            case MbufMode::LinearAccess:
                g->mbuf_mode = MbufMode::LinearAccess;
                break;
            }
            break;

        case 'r':
#ifdef RATE_LIMITING_CONSUMER
            g->cons_rate_limit_cycles = atoi(optarg);
            if (g->cons_rate_limit_cycles < 0) {
                printf("    Invalid consumer rate limit '%s'\n", optarg);
                return -1;
            }
#else
            printf("    Rate limiting consumer not supported\n");
            return -1;
#endif
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

        case 'w':
            g->deceive_hw_data_prefetcher = true;
            break;

        case 'H':
            g->hugepages = true;
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    tsc_init();
    g->cons_rate_limit_cycles = ns2tsc(g->cons_rate_limit_cycles);
    g->prod_spin_ticks        = ns2tsc(g->prod_spin_ticks);
    g->cons_spin_ticks        = ns2tsc(g->cons_spin_ticks);
    run_test(g);

    return 0;
}
