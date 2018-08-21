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
struct worker {
    struct traffic_analyzer *ta;
    pthread_t th;
    struct mbuf *pool;
    unsigned int mbuf_next;
    unsigned int pool_mbufs;
    struct Blq *blq;
    struct Iffq *ffq;
};

typedef unsigned int (*enq_t)(struct worker *w, unsigned int batch);
typedef void (*deq_t)(struct worker *w, unsigned int batch);

struct traffic_analyzer {
    /* Type of spsc queue to be used. */
    const char *qtype;

    /* Number of (consumer) threads performing traffic analysis. */
    unsigned int num_analyzers;

    /* Length of each SPSC queue. */
    unsigned int qlen;

    /* Load balancer thread (producer). */
    pthread_t lb_th;

    /* Producer work. */
    enq_t enq;

    /* Consumer work. */
    deq_t deq;

    /* Analyzer threads. */
    struct worker *workers;
};

static size_t
worker_pool_size(struct traffic_analyzer *ta)
{
    return ALIGNED_SIZE(sizeof(struct mbuf) * ta->qlen * 2);
}

static size_t
worker_pool_mbufs(struct traffic_analyzer *ta)
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

/* Enqueue and dequeue wrappers. */
static unsigned int
lq_enq(struct worker *w, unsigned int batch)
{
    unsigned int mbuf_next = w->mbuf_next;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = w->pool + mbuf_next;

        udp_60_bytes_packet_get(m);
        if (lq_write(w->blq, (uintptr_t)m)) {
            break;
        }
        if (++mbuf_next == w->pool_mbufs) {
            mbuf_next = 0;
        }
    }
    w->mbuf_next = mbuf_next;

    return count;
}

static void
lq_deq(struct worker *w, unsigned batch)
{
    for (; batch > 0; batch--) {
        struct mbuf *m = (struct mbuf *)lq_read(w->blq);

        if (m == NULL) {
            return;
        }
        analyze_mbuf(m);
    }
}

/*
static int
blq_enq(struct worker *w, struct mbuf *m)
{
    struct Blq *blq = w->blq;
    unsigned int wspace = blq_wspace(blq);

    if (wspace == 0) {
        return -1;
    }

    blq_write_local(blq, (uintptr_t)m);
    blq_write_publish(blq);

    return 0;
}

static struct mbuf *
blq_deq(struct worker *w)
{
    struct Blq *blq = w->blq;
    unsigned int rspace = blq_rspace(blq);
    struct mbuf *m;

    if (rspace == 0) {
        return NULL;
    }

    m = blq_read_local(blq);

}
*/

static int stop = 0;

static void *
lb(void *opaque)
{
    struct traffic_analyzer *ta = opaque;
    unsigned int num_consumers  = ta->num_analyzers;
    unsigned int batch          = 1;
    unsigned int i              = 0;
    enq_t enq                   = ta->enq;
    unsigned long long count    = 0;
    struct timespec t_start, t_end;

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        struct worker *w = ta->workers + i;

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
    }

    return NULL;
}

static void *
analyze(void *opaque)
{
    struct worker *w   = opaque;
    deq_t deq          = w->ta->deq;
    unsigned int batch = 1;

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
           "    [-l SPSC_QUEUES_LEN = 256]\n"
           "    [-t QUEUE_TYPE(lq,llq,blq,ffq,iffq,biffq) = lq]\n",
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
    ta->num_analyzers = 2;
    ta->qlen          = 256;
    ta->qtype         = "lq";
    ffq               = 0;

    while ((opt = getopt(argc, argv, "hn:l:t:")) != -1) {
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

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    qsize = ffq ? iffq_size(ta->qlen) : blq_size(ta->qlen);

    if (!strcmp(ta->qtype, "lq")) {
        ta->enq = lq_enq;
        ta->deq = lq_deq;
    } else if (!strcmp(ta->qtype, "llq")) {
        ta->enq = lq_enq;
        ta->deq = lq_deq;
    }

    /*
     * Setup phase.
     */
    {
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;

        memory_size = ta->num_analyzers * (worker_pool_size(ta) + qsize);
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

    ta->workers = szalloc(ta->num_analyzers * sizeof(ta->workers[0]));

    {
        char *memory_cursor = memory;

        for (i = 0; i < ta->num_analyzers; i++) {
            struct worker *w = ta->workers + i;

            w->ta         = ta;
            w->pool       = (struct mbuf *)memory_cursor;
            w->mbuf_next  = 0;
            w->pool_mbufs = worker_pool_mbufs(ta);
            memory_cursor += worker_pool_size(ta);
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

    for (i = 0; i < ta->num_analyzers; i++) {
        struct worker *w = ta->workers + i;
        if (pthread_create(&w->th, NULL, analyze, w)) {
            printf("pthread_create(worker) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    if (pthread_create(&ta->lb_th, NULL, lb, ta)) {
        printf("pthread_create(lb) failed\n");
        exit(EXIT_FAILURE);
    }

    /*
     * Teardown phase.
     */
    for (i = 0; i < ta->num_analyzers; i++) {
        struct worker *w = ta->workers + i;
        if (pthread_join(w->th, NULL)) {
            printf("pthread_join(worker) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    if (pthread_join(ta->lb_th, NULL)) {
        printf("pthread_join(lb) failed\n");
        exit(EXIT_FAILURE);
    }

    free(ta->workers);
    munmap(memory, memory_size);

    return 0;
}
