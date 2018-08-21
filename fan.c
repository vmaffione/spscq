#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/prctl.h>

#include "mlib.h"
#include "spscq.h"

struct mbuf {
    uint32_t len;
    char buf[60];
};

/* Forward declaration */
struct experiment;

/* Context of a root thread (load balancer or transmission). */
struct root {
    struct experiment *ce;
    pthread_t th;
    unsigned int first_leaf;
    unsigned int num_leaves;
    struct mbuf sink[1];
    double mpps;
};

/* Context of a leaf thread (traffic analyzer or sender clients). */
struct leaf {
    struct experiment *ce;
    struct root *root;
    pthread_t th;
    struct mbuf *pool;
    unsigned int mbuf_next;
    unsigned int pool_mbufs;
    struct Blq *blq;
    struct Iffq *ffq;
};

typedef unsigned int (*root_func_t)(struct leaf *w, unsigned int batch);
typedef void (*leaf_func_t)(struct leaf *w, unsigned int batch);

struct experiment {
    /* Experiment name. */
    const char *expname;

    /* Type of spsc queue to be used. */
    const char *qtype;

    /* Number of (root) threads performing load balancing or transmission. */
    unsigned int num_roots;

    /* Number of (leaf) threads performing traffic analysis or sender clients.
     */
    unsigned int num_leaves;

    /* Length of each SPSC queue. */
    unsigned int qlen;

    /* Batch size (in packets) for root and leaf operation. */
    unsigned int batch;

    /* Root work. */
    root_func_t root_func;

    /* Leaf work. */
    leaf_func_t leaf_func;

    /* Leaf nodes. */
    struct leaf *leaves;

    /* Root nodes. */
    struct root *roots;
};

static size_t
leaf_pool_size(struct experiment *ce)
{
    return ALIGNED_SIZE(sizeof(struct mbuf) * ce->qlen * 2);
}

static size_t
leaf_pool_mbufs(struct experiment *ce)
{
    return ce->qlen * 2;
}

/* Alloc zeroed cacheline-aligned memory, aborting on failure. */
static void *
szalloc(size_t size)
{
    void *p = NULL;
    int ret = posix_memalign(&p, ALIGN_SIZE, size);
    if (ret) {
        printf("allocation failure: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    memset(p, 0, size);
    return p;
}

static void
timerslack_reset(void)
{
    if (prctl(PR_SET_TIMERSLACK, /*nanoseconds=*/1)) {
        printf("Failed to set the timerslack!\n");
    }
}

static const uint8_t bytes[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x45, 0x10, 0x00, 0x2e, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
    0x26, 0xad, 0x0a, 0x00, 0x00, 0x01, 0x0a, 0x01, 0x00, 0x01, 0x04, 0xd2,
    0x04, 0xd2, 0x00, 0x1a, 0x15, 0x80, 0x6e, 0x65, 0x74, 0x6d, 0x61, 0x70,
    0x20, 0x70, 0x6b, 0x74, 0x2d, 0x67, 0x65, 0x6e, 0x20, 0x44, 0x49, 0x52};

static inline void
udp_60_bytes_packet_get(struct mbuf *m)
{
    assert(sizeof(bytes) == 60);
    m->len = sizeof(bytes);
    memcpy(m->buf, bytes, sizeof(m->buf));
}

static void
analyze_mbuf(struct mbuf *m)
{
    unsigned int sum = 0;
    int i;
    int k;

    for (k = 0; k < 5; k++) {
        for (i = 0; i < m->len; i++) {
            if (m->buf[i] > 31 + k) {
                sum += m->buf[i];
            } else {
                sum -= m->buf[i];
            }
        }
    }
    if (sum == 281) {
        printf("WOW\n");
    }
}

/*
 * Per-queue root and leaf functions.
 */

static unsigned int
lq_root_lb(struct leaf *w, unsigned int batch)
{
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        if (lq_write(w->blq, (uintptr_t)m)) {
            break;
        }
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
    }
    w->mbuf_next = mbuf_next;

    return count;
}

static void
lq_leaf_analyze(struct leaf *w, unsigned batch)
{
    for (; batch > 0; batch--) {
        struct mbuf *m = (struct mbuf *)lq_read(w->blq);

        if (m == NULL) {
            return;
        }
        analyze_mbuf(m);
    }
}

static unsigned int
llq_root_lb(struct leaf *w, unsigned int batch)
{
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        if (llq_write(w->blq, (uintptr_t)m)) {
            break;
        }
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
    }
    w->mbuf_next = mbuf_next;

    return count;
}

static void
llq_leaf_analyze(struct leaf *w, unsigned batch)
{
    for (; batch > 0; batch--) {
        struct mbuf *m = (struct mbuf *)llq_read(w->blq);

        if (m == NULL) {
            return;
        }
        analyze_mbuf(m);
    }
}

static unsigned int
blq_root_lb(struct leaf *w, unsigned int batch)
{
    struct Blq *blq        = w->blq;
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int wspace    = blq_wspace(blq);
    unsigned int count;

    if (batch > wspace) {
        batch = wspace;
    }

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        blq_write_local(blq, (uintptr_t)m);
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
    }

    blq_write_publish(blq);
    w->mbuf_next = mbuf_next;

    return count;
}

static void
blq_leaf_analyze(struct leaf *w, unsigned batch)
{
    struct Blq *blq     = w->blq;
    unsigned int rspace = blq_rspace(blq);

    if (batch > rspace) {
        batch = rspace;
    }
    for (; batch > 0; batch--) {
        struct mbuf *m = (struct mbuf *)blq_read_local(blq);
        analyze_mbuf(m);
    }
    blq_read_publish(blq);
}

static unsigned int
ffq_root_lb(struct leaf *w, unsigned int batch)
{
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        if (ffq_write(w->ffq, (uintptr_t)m)) {
            break;
        }
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
    }
    w->mbuf_next = mbuf_next;

    return count;
}

static void
ffq_leaf_analyze(struct leaf *w, unsigned batch)
{
    for (; batch > 0; batch--) {
        struct mbuf *m = (struct mbuf *)ffq_read(w->ffq);

        if (m == NULL) {
            return;
        }
        analyze_mbuf(m);
    }
}

static unsigned int
iffq_root_lb(struct leaf *w, unsigned int batch)
{
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        if (iffq_insert(w->ffq, (uintptr_t)m)) {
            break;
        }
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
    }
    w->mbuf_next = mbuf_next;

    return count;
}

static void
iffq_leaf_analyze(struct leaf *w, unsigned batch)
{
    struct Iffq *ffq = w->ffq;

    for (; batch > 0; batch--) {
        struct mbuf *m = (struct mbuf *)iffq_extract(ffq);

        if (m == NULL) {
            return;
        }
        analyze_mbuf(m);
    }
    iffq_clear(ffq);
}

static unsigned int
biffq_root_lb(struct leaf *w, unsigned int batch)
{
    struct Iffq *ffq       = w->ffq;
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int wspace    = iffq_wspace(ffq);
    unsigned int count;

    if (batch > wspace) {
        batch = wspace;
    }

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        iffq_insert_local(ffq, (uintptr_t)m);
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
    }

    iffq_insert_publish(ffq);
    w->mbuf_next = mbuf_next;

    return count;
}

static unsigned int
lq_root_transmitter(struct leaf *w, unsigned int batch)
{
    volatile struct mbuf *dst = &w->root->sink[0];
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *src = (struct mbuf *)lq_read(w->blq);

        if (!src) {
            break;
        }

        dst->len = src->len;
        memcpy((void *)dst->buf, src->buf, src->len);
    }

    return count;
}

static void
lq_leaf_sender(struct leaf *w, unsigned int batch)
{
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        if (lq_write(w->blq, (uintptr_t)m)) {
            break;
        }
        if (unlikely(++mbuf_next == w->pool_mbufs)) {
            mbuf_next = 0;
        }
        usleep(100);
    }
    w->mbuf_next = mbuf_next;
}

static int stop = 0;

static void *
root_worker(void *opaque)
{
    struct root *p           = opaque;
    struct leaf *first_leaf  = p->ce->leaves + p->first_leaf;
    unsigned lb_idx          = (unsigned int)(p - p->ce->roots);
    unsigned int num_leaves  = p->num_leaves;
    unsigned int batch       = p->ce->batch;
    root_func_t root_func    = p->ce->root_func;
    unsigned int i           = 0;
    unsigned long long count = 0;
    struct timespec t_start, t_end;

    timerslack_reset();

    printf("root %u handles %u leaves\n", lb_idx, num_leaves);
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        struct leaf *w = first_leaf + i;

        count += root_func(w, batch);
        if (++i == num_leaves) {
            i = 0;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    {
        unsigned long long ns =
            1000000000ULL * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);
        double rate = (double)count * 1000.0 / (double)ns;
        printf("root throughput: %.3f Mpps\n", rate);
        p->mpps = rate;
    }

    return NULL;
}

static void *
leaf_worker(void *opaque)
{
    struct leaf *w        = opaque;
    leaf_func_t leaf_func = w->ce->leaf_func;
    unsigned int batch    = w->ce->batch;

    timerslack_reset();

    while (!ACCESS_ONCE(stop)) {
        leaf_func(w, batch);
    }

    return NULL;
}

static void
analyzer_benchmark(void)
{
    struct timespec t_start, t_end;
    unsigned long long count = 0;
    struct mbuf m;

    udp_60_bytes_packet_get(&m);
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        analyze_mbuf(&m);
        count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    {
        unsigned long long ns =
            1000000000ULL * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);
        double rate = (double)count * 1000.0 / (double)ns;
        printf("Consumer benchmark: %.3f Mpps\n", rate);
    }
}

static void
sigint_handler(int signum)
{
    ACCESS_ONCE(stop) = 1;
}

static void
usage(const char *progname)
{
    printf("%s\n"
           "    [-h (show this help and exit)]\n"
           "    [-e EXPNAME = (fanout, fanin)]\n"
           "    [-n NUM_LEAVES = 2]\n"
           "    [-N NUM_ROOTS = 1]\n"
           "    [-l SPSC_QUEUES_LEN = 256]\n"
           "    [-t QUEUE_TYPE(lq,llq,blq,ffq,iffq,biffq) = lq]\n"
           "    [-b BATCH_LENGTH = 8]\n"
           "    [-j (run leaf benchmark)]\n",
           progname);
}

int
main(int argc, char **argv)
{
    struct experiment _ce;
    struct experiment *ce = &_ce;
    size_t memory_size    = 0;
    size_t qsize          = 0;
    char *memory          = NULL;
    int opt;
    int ffq;           /* boolean */
    int benchmark = 0; /* boolean */
    int i;

    {
        struct sigaction sa;

        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(SIGINT, &sa, NULL)) {
            perror("sigaction(SIGINT)");
            exit(EXIT_FAILURE);
        }
    }

    memset(ce, 0, sizeof(*ce));
    ce->num_roots  = 1;
    ce->num_leaves = 2;
    ce->qlen       = 256;
    ce->expname    = "fanout";
    ce->qtype      = "lq";
    ce->batch      = 8;
    ffq            = 0;

    while ((opt = getopt(argc, argv, "hn:l:t:b:N:je:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            ce->num_leaves = atoi(optarg);
            if (ce->num_leaves == 0 || ce->num_leaves > 1000) {
                printf("    Invalid number of analyzers '%s'\n", optarg);
                return -1;
            }
            break;

        case 'N':
            ce->num_roots = atoi(optarg);
            if (ce->num_roots == 0 || ce->num_roots > 1000) {
                printf("    Invalid number of load balancers '%s'\n", optarg);
                return -1;
            }
            break;

        case 'l':
            ce->qlen = atoi(optarg);
            if (ce->qlen % sizeof(uintptr_t) != 0 || ce->qlen == 0 ||
                ce->qlen > 8192) {
                printf("    Invalid queue length '%s'\n", optarg);
                return -1;
            }
            break;

        case 't':
            if (!strcmp("lq", optarg) || !strcmp("llq", optarg) ||
                !strcmp("blq", optarg)) {
                ffq = 0;
            } else if (!strcmp("ffq", optarg) || !strcmp("iffq", optarg) ||
                       !strcmp("biffq", optarg)) {
                ffq = 1;
            } else {
                printf("    Invalid queue type %s\n", optarg);
                return -1;
            }
            ce->qtype = optarg;
            break;

        case 'b':
            ce->batch = atoi(optarg);
            if (ce->batch < 1 || ce->batch > 8192) {
                printf("    Invalid batch length '%s'\n", optarg);
                return -1;
            }
            break;

        case 'j':
            benchmark = 1;
            break;

        case 'e':
            if (strcmp(optarg, "fanout") && strcmp(optarg, "fanin")) {
                printf("    Invalid experiment name '%s'\n", optarg);
                return -1;
            }
            ce->expname = optarg;
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    if (ce->num_roots > ce->num_leaves) {
        printf("Invalid parameters: num_leaves must be "
               ">= num_roots\n");
        return -1;
    }

    if (benchmark) {
        printf("Running leaf benchmark: CTRL-C to stop\n");
        analyzer_benchmark();
        return 0;
    }

    qsize = ffq ? iffq_size(ce->qlen) : blq_size(ce->qlen);

    if (!strcmp(ce->expname, "fanout")) {
        if (!strcmp(ce->qtype, "lq")) {
            ce->root_func = lq_root_lb;
            ce->leaf_func = lq_leaf_analyze;
        } else if (!strcmp(ce->qtype, "llq")) {
            ce->root_func = llq_root_lb;
            ce->leaf_func = llq_leaf_analyze;
        } else if (!strcmp(ce->qtype, "blq")) {
            ce->root_func = blq_root_lb;
            ce->leaf_func = blq_leaf_analyze;
        } else if (!strcmp(ce->qtype, "ffq")) {
            ce->root_func = ffq_root_lb;
            ce->leaf_func = ffq_leaf_analyze;
        } else if (!strcmp(ce->qtype, "iffq")) {
            ce->root_func = iffq_root_lb;
            ce->leaf_func = iffq_leaf_analyze;
        } else if (!strcmp(ce->qtype, "biffq")) {
            ce->root_func = biffq_root_lb;
            ce->leaf_func = iffq_leaf_analyze;
        }
    } else {
        if (!strcmp(ce->qtype, "lq")) {
            ce->root_func = lq_root_transmitter;
            ce->leaf_func = lq_leaf_sender;
        } else {
            printf("Not implemented yet ...\n");
            return -1;
        }
    }

    /*
     * Setup phase.
     */
    {
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;

        memory_size = ce->num_leaves * (leaf_pool_size(ce) + qsize);
        printf("Allocating %lu bytes\n", (long unsigned int)memory_size);
        for (;;) {
            memory = mmap(NULL, memory_size, PROT_WRITE | PROT_READ, mmap_flags,
                          -1, 0);
            if (memory != MAP_FAILED) {
                break;
            }
            if (!(mmap_flags & MAP_HUGETLB)) {
                perror("mmap(memory)\n");
                exit(EXIT_FAILURE);
            }
            mmap_flags &= ~MAP_HUGETLB;
            printf("WARNING: Cannot allocate hugepages: falling back to "
                   "regular pages\n");
        }
    }

    ce->leaves = szalloc(ce->num_leaves * sizeof(ce->leaves[0]));
    ce->roots  = szalloc(ce->num_roots * sizeof(ce->roots[0]));

    {
        char *memory_cursor = memory;

        for (i = 0; i < ce->num_leaves; i++) {
            struct leaf *w = ce->leaves + i;

            w->ce         = ce;
            w->pool       = (struct mbuf *)memory_cursor;
            w->mbuf_next  = 0;
            w->pool_mbufs = leaf_pool_mbufs(ce);
            memory_cursor += leaf_pool_size(ce);
            if (!ffq) {
                w->blq = (struct Blq *)memory_cursor;
                blq_init(w->blq, ce->qlen);
            } else {
                w->ffq = (struct Iffq *)memory_cursor;
                iffq_init(w->ffq, ce->qlen, 32 * sizeof(w->ffq->q[0]),
                          /*improved=*/!strcmp(ce->qtype, "iffq"));
            }
            memory_cursor += qsize;
        }
    }

    {
        unsigned int stride =
            (ce->num_leaves + ce->num_roots - 1) / ce->num_roots;
        unsigned int overflow  = stride * ce->num_roots - ce->num_leaves;
        unsigned int next_leaf = 0;

        for (i = 0; i < ce->num_roots; i++) {
            struct root *p = ce->roots + i;
            int j;
            p->ce         = ce;
            p->first_leaf = next_leaf;
            p->num_leaves = (i < overflow) ? (stride - 1) : stride;
            next_leaf += p->num_leaves;
            for (j = 0; j < p->num_leaves; j++) {
                struct leaf *l = ce->leaves + p->first_leaf + j;
                l->root        = p;
            }
        }
    }

    for (i = 0; i < ce->num_leaves; i++) {
        struct leaf *w = ce->leaves + i;

        if (pthread_create(&w->th, NULL, leaf_worker, w)) {
            printf("pthread_create(leaf) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ce->num_roots; i++) {
        struct root *p = ce->roots + i;

        if (pthread_create(&p->th, NULL, root_worker, p)) {
            printf("pthread_create(root) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    printf("Press CTRL-C to stop\n");

    /*
     * Teardown phase.
     */
    for (i = 0; i < ce->num_roots; i++) {
        struct root *p = ce->roots + i;

        if (pthread_join(p->th, NULL)) {
            printf("pthread_join(root) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ce->num_leaves; i++) {
        struct leaf *w = ce->leaves + i;

        if (pthread_join(w->th, NULL)) {
            printf("pthread_join(leaf) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    {
        double tot_mpps = 0.0;

        for (i = 0; i < ce->num_roots; i++) {
            struct root *p = ce->roots + i;

            tot_mpps += p->mpps;
        }

        printf("Total rate %.3f Mpps\n", tot_mpps);
    }

    free(ce->leaves);
    free(ce->roots);
    munmap(memory, memory_size);

    return 0;
}
