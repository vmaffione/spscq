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

#include "mlib.h"
#include "spscq.h"

struct mbuf {
    uint32_t len;
    char buf[60];
};

/* Forward declaration */
struct traffic_analyzer;

/* Context of a traffic analyzer thread (consumer). */
struct consumer {
    struct traffic_analyzer *ta;
    pthread_t th;
    struct mbuf *pool;
    unsigned int mbuf_next;
    unsigned int pool_mbufs;
    struct Blq *blq;
    struct Iffq *ffq;
};

struct producer {
    struct traffic_analyzer *ta;
    pthread_t th;
    unsigned int first_analyzer;
    unsigned int num_analyzers;
    double mpps;
};

typedef unsigned int (*enq_t)(struct consumer *w, unsigned int batch);
typedef void (*deq_t)(struct consumer *w, unsigned int batch);

struct traffic_analyzer {
    /* Type of spsc queue to be used. */
    const char *qtype;

    /* Number of (producer) threads performing load balancing. */
    unsigned int num_load_balancers;

    /* Number of (consumer) threads performing traffic analysis. */
    unsigned int num_analyzers;

    /* Length of each SPSC queue. */
    unsigned int qlen;

    /* Batch size (in packets) for producer and consumer operation. */
    unsigned int batch;

    /* Producer work. */
    enq_t enq;

    /* Consumer work. */
    deq_t deq;

    /* Analyzer threads. */
    struct consumer *consumers;

    /* Load balancer threads. */
    struct producer *producers;
};

static size_t
consumer_pool_size(struct traffic_analyzer *ta)
{
    return ALIGNED_SIZE(sizeof(struct mbuf) * ta->qlen * 2);
}

static size_t
consumer_pool_mbufs(struct traffic_analyzer *ta)
{
    return ta->qlen * 2;
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
 * Enqueue and dequeue wrappers.
 */

static unsigned int
lq_enq(struct consumer *w, unsigned int batch)
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
lq_deq(struct consumer *w, unsigned batch)
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
llq_enq(struct consumer *w, unsigned int batch)
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
llq_deq(struct consumer *w, unsigned batch)
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
blq_enq(struct consumer *w, unsigned int batch)
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
blq_deq(struct consumer *w, unsigned batch)
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
ffq_enq(struct consumer *w, unsigned int batch)
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
ffq_deq(struct consumer *w, unsigned batch)
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
iffq_enq(struct consumer *w, unsigned int batch)
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
iffq_deq(struct consumer *w, unsigned batch)
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
biffq_enq(struct consumer *w, unsigned int batch)
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

static int stop = 0;

static void *
lb(void *opaque)
{
    struct producer *p              = opaque;
    struct consumer *first_consumer = p->ta->consumers + p->first_analyzer;
    unsigned lb_idx                 = (unsigned int)(p - p->ta->producers);
    unsigned int num_consumers      = p->num_analyzers;
    unsigned int batch              = p->ta->batch;
    enq_t enq                       = p->ta->enq;
    unsigned int i                  = 0;
    unsigned long long count        = 0;
    struct timespec t_start, t_end;

    printf("lb %u handles %u analyzers\n", lb_idx, num_consumers);
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        struct consumer *w = first_consumer + i;

        count += enq(w, batch);
        if (++i == num_consumers) {
            i = 0;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    {
        unsigned long long ns =
            1000000000ULL * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);
        double rate = (double)count * 1000.0 / (double)ns;
        printf("lb throughput: %.3f Mpps\n", rate);
        p->mpps = rate;
    }

    return NULL;
}

static void *
analyze(void *opaque)
{
    struct consumer *w = opaque;
    deq_t deq          = w->ta->deq;
    unsigned int batch = w->ta->batch;

    while (!ACCESS_ONCE(stop)) {
        deq(w, batch);
    }

    return NULL;
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
           "    [-n NUM_ANALYZERS = 2]\n"
           "    [-N NUM_LOAD_BALANCERS = 1]\n"
           "    [-l SPSC_QUEUES_LEN = 256]\n"
           "    [-t QUEUE_TYPE(lq,llq,blq,ffq,iffq,biffq) = lq]\n"
           "    [-b BATCH_LENGTH = 8]\n",
           progname);
}

int
main(int argc, char **argv)
{
    struct traffic_analyzer _ta;
    struct traffic_analyzer *ta = &_ta;
    size_t memory_size          = 0;
    size_t qsize                = 0;
    char *memory                = NULL;
    int opt;
    int ffq; /* boolean */
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

    memset(ta, 0, sizeof(*ta));
    ta->num_load_balancers = 1;
    ta->num_analyzers      = 2;
    ta->qlen               = 256;
    ta->qtype              = "lq";
    ta->batch              = 8;
    ffq                    = 0;

    while ((opt = getopt(argc, argv, "hn:l:t:b:N:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            ta->num_analyzers = atoi(optarg);
            if (ta->num_analyzers == 0 || ta->num_analyzers > 1000) {
                printf("    Invalid number of analyzers '%s'\n", optarg);
                return -1;
            }
            break;

        case 'N':
            ta->num_load_balancers = atoi(optarg);
            if (ta->num_load_balancers == 0 || ta->num_load_balancers > 1000) {
                printf("    Invalid number of load balancers '%s'\n", optarg);
                return -1;
            }
            break;

        case 'l':
            ta->qlen = atoi(optarg);
            if (ta->qlen % sizeof(uintptr_t) != 0 || ta->qlen == 0 ||
                ta->qlen > 8192) {
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
            ta->qtype = optarg;
            break;

        case 'b':
            ta->batch = atoi(optarg);
            if (ta->batch < 1 || ta->batch > 8192) {
                printf("    Invalid batch length '%s'\n", optarg);
                return -1;
            }
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    if (ta->num_load_balancers > ta->num_analyzers) {
        printf("Invalid parameters: num_analyzers must be "
               ">= num_load_balancers\n");
        return -1;
    }

    qsize = ffq ? iffq_size(ta->qlen) : blq_size(ta->qlen);

    if (!strcmp(ta->qtype, "lq")) {
        ta->enq = lq_enq;
        ta->deq = lq_deq;
    } else if (!strcmp(ta->qtype, "llq")) {
        ta->enq = llq_enq;
        ta->deq = llq_deq;
    } else if (!strcmp(ta->qtype, "blq")) {
        ta->enq = blq_enq;
        ta->deq = blq_deq;
    } else if (!strcmp(ta->qtype, "ffq")) {
        ta->enq = ffq_enq;
        ta->deq = ffq_deq;
    } else if (!strcmp(ta->qtype, "iffq")) {
        ta->enq = iffq_enq;
        ta->deq = iffq_deq;
    } else if (!strcmp(ta->qtype, "biffq")) {
        ta->enq = biffq_enq;
        ta->deq = iffq_deq;
    }

    /*
     * Setup phase.
     */
    {
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;

        memory_size = ta->num_analyzers * (consumer_pool_size(ta) + qsize);
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

    ta->consumers = szalloc(ta->num_analyzers * sizeof(ta->consumers[0]));
    ta->producers = szalloc(ta->num_load_balancers * sizeof(ta->producers[0]));

    {
        char *memory_cursor = memory;

        for (i = 0; i < ta->num_analyzers; i++) {
            struct consumer *w = ta->consumers + i;

            w->ta         = ta;
            w->pool       = (struct mbuf *)memory_cursor;
            w->mbuf_next  = 0;
            w->pool_mbufs = consumer_pool_mbufs(ta);
            memory_cursor += consumer_pool_size(ta);
            if (!ffq) {
                w->blq = (struct Blq *)memory_cursor;
                blq_init(w->blq, ta->qlen);
            } else {
                w->ffq = (struct Iffq *)memory_cursor;
                iffq_init(w->ffq, ta->qlen, 32 * sizeof(w->ffq->q[0]),
                          /*improved=*/!strcmp(ta->qtype, "iffq"));
            }
            memory_cursor += qsize;
        }
    }

    {
        unsigned int stride = (ta->num_analyzers + ta->num_load_balancers - 1) /
                              ta->num_load_balancers;
        unsigned int overflow =
            stride * ta->num_load_balancers - ta->num_analyzers;
        unsigned int next_analyzer = 0;

        for (i = 0; i < ta->num_load_balancers; i++) {
            struct producer *p = ta->producers + i;
            p->ta              = ta;
            p->first_analyzer  = next_analyzer;
            p->num_analyzers   = (i < overflow) ? (stride - 1) : stride;
            next_analyzer += p->num_analyzers;
        }
    }

    for (i = 0; i < ta->num_analyzers; i++) {
        struct consumer *w = ta->consumers + i;

        if (pthread_create(&w->th, NULL, analyze, w)) {
            printf("pthread_create(consumer) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ta->num_load_balancers; i++) {
        struct producer *p = ta->producers + i;

        if (pthread_create(&p->th, NULL, lb, p)) {
            printf("pthread_create(lb) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * Teardown phase.
     */
    for (i = 0; i < ta->num_load_balancers; i++) {
        struct producer *p = ta->producers + i;

        if (pthread_join(p->th, NULL)) {
            printf("pthread_join(producer) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ta->num_analyzers; i++) {
        struct consumer *w = ta->consumers + i;

        if (pthread_join(w->th, NULL)) {
            printf("pthread_join(consumer) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    {
        double tot_mpps = 0.0;

        for (i = 0; i < ta->num_load_balancers; i++) {
            struct producer *p = ta->producers + i;

            tot_mpps += p->mpps;
        }

        printf("Total rate %.3f Mpps\n", tot_mpps);
    }

    free(ta->consumers);
    free(ta->producers);
    munmap(memory, memory_size);

    return 0;
}
