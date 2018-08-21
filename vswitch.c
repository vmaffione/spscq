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
#include <arpa/inet.h>

#include "mlib.h"
#include "spscq.h"

#define DEBUG
#undef DEBUG

struct mbuf {
    uint32_t len;
    char dst_mac[6];
    char src_mac[6];
    uint16_t ethtype;
    char payload[0];
};

static struct mbuf *
mbuf_alloc(size_t iplen, unsigned int src_idx, unsigned int dst_idx)
{
    struct mbuf *m = malloc(sizeof(struct mbuf) + iplen);
    uint16_t *macp;

    if (unlikely(!m)) {
        exit(EXIT_FAILURE);
    }
    m->len     = 14 + iplen;
    macp       = (uint16_t *)&m->dst_mac[4];
    *macp      = htons(dst_idx);
    macp       = (uint16_t *)&m->src_mac[4];
    *macp      = htons(src_idx);
    m->ethtype = htons(0x0800);
    memset(m->payload, 0x45, iplen);

    return m;
}

static void
mbuf_free(struct mbuf *m)
{
    free(m);
}

/* Forward declaration */
struct vswitch_experiment;

/* Context of a vswitch thread (load balancer or transmission). */
struct vswitch {
    struct vswitch_experiment *ce;
    pthread_t th;
    unsigned int first_client;
    unsigned int num_clients;
    struct mbuf **mbufs;
    double mpps;
};

/* Context of a client thread (traffic analyzer or sender clients). */
struct client {
    struct vswitch_experiment *ce;
    struct vswitch *vswitch;
    pthread_t th;
    unsigned int idx;
#define TXQ 0
#define RXQ 1
#define MAXQ 2
    struct Blq *blq[MAXQ];
    struct Iffq *ffq[MAXQ];
};

typedef unsigned int (*vswitch_func_t)(struct client *c, unsigned int batch);
typedef int (*client_func_t)(struct client *c, struct mbuf *m);

struct vswitch_experiment {
    /* Type of spsc queue to be used. */
    const char *qtype;

    /* Number of (vswitch) threads performing load balancing or transmission. */
    unsigned int num_vswitchs;

    /* Number of (client) threads performing traffic analysis or sender clients.
     */
    unsigned int num_clients;

    /* Length of each SPSC queue. */
    unsigned int qlen;

    /* Length of each mbuf IP payload. */
    unsigned iplen;

    /* Batch size (in packets) for vswitch and client operation. */
    unsigned int vswitch_batch;
    unsigned int client_batch;

    /* Vswitch work. */
    vswitch_func_t vswitch_func;

    /* Client work. */
    client_func_t client_func;

    /* Leaf nodes. */
    struct client *clients;

    /* Root nodes. */
    struct vswitch *vswitchs;

    /* Microseconds for sender usleep(). */
    unsigned int client_usleep;
};

static inline struct client *
dst_client(struct client *c, struct mbuf *m)
{
    uint16_t dst_idx = ntohs(*((uint16_t *)&m->dst_mac[4]));

#ifdef DEBUG
    printf("send %u --> %u\n", c->idx, dst_idx);
#endif /* DEBUG */

    return c->ce->clients + dst_idx;
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

/*
 * Queue-specific client and vswitch implementations.
 */

static int
lq_client(struct client *c, struct mbuf *sm)
{
    struct mbuf *m;

    while ((m = (struct mbuf *)lq_read(c->blq[RXQ])) != NULL) {
        mbuf_free(m);
    }

    return lq_write(c->blq[TXQ], (uintptr_t)sm);
}

static unsigned int
lq_vswitch(struct client *c, unsigned int batch)
{
    struct mbuf **mbufs = c->vswitch->mbufs;
    unsigned int count;
    unsigned int i;

    for (count = 0; count < batch; count++) {
        mbufs[count] = (struct mbuf *)lq_read(c->blq[TXQ]);
        if (mbufs[count] == NULL) {
            break;
        }
    }

    for (i = 0; i < count; i++) {
        struct mbuf *m    = mbufs[i];
        struct client *dc = dst_client(c, m);

        if (lq_write(dc->blq[RXQ], (uintptr_t)m)) {
            mbuf_free(m);
        }
    }

    return count;
}

static int
llq_client(struct client *c, struct mbuf *sm)
{
    struct mbuf *m;

    while ((m = (struct mbuf *)llq_read(c->blq[RXQ])) != NULL) {
        mbuf_free(m);
    }

    return llq_write(c->blq[TXQ], (uintptr_t)sm);
}

static unsigned int
llq_vswitch(struct client *c, unsigned int batch)
{
    struct mbuf **mbufs = c->vswitch->mbufs;
    unsigned int count;
    unsigned int i;

    for (count = 0; count < batch; count++) {
        mbufs[count] = (struct mbuf *)llq_read(c->blq[TXQ]);
        if (mbufs[count] == NULL) {
            break;
        }
    }

    for (i = 0; i < count; i++) {
        struct mbuf *m    = mbufs[i];
        struct client *dc = dst_client(c, m);

        if (llq_write(dc->blq[RXQ], (uintptr_t)m)) {
            mbuf_free(m);
        }
    }

    return count;
}

static int
blq_client(struct client *c, struct mbuf *sm)
{
    unsigned int rspace = blq_rspace(c->blq[RXQ]);

    for (; rspace > 0; rspace--) {
        struct mbuf *m = (struct mbuf *)blq_read_local(c->blq[RXQ]);
        mbuf_free(m);
    }
    blq_read_publish(c->blq[RXQ]);

    if (blq_wspace(c->blq[TXQ]) == 0) {
        return -1;
    }

    blq_write_local(c->blq[TXQ], (uintptr_t)sm);
    blq_write_publish(c->blq[TXQ]);

    return 0;
}

static unsigned int
blq_vswitch(struct client *c, unsigned int batch)
{
    struct mbuf **mbufs = c->vswitch->mbufs;
    unsigned int rspace = blq_rspace(c->blq[TXQ]);
    unsigned int i;

    if (batch > rspace) {
        batch = rspace;
    }

    for (i = 0; i < batch; i++) {
        mbufs[i] = (struct mbuf *)blq_read_local(c->blq[TXQ]);
    }
    blq_read_publish(c->blq[TXQ]);

    for (i = 0; i < batch; i++) {
        struct mbuf *m    = mbufs[i];
        struct client *dc = dst_client(c, m);
        struct Blq *dblq  = dc->blq[RXQ];

        if (blq_wspace(dblq) == 0) {
            mbuf_free(m);
        } else {
            blq_write_local(dblq, (uintptr_t)m);
            blq_write_publish(dblq);
        }
    }

    return batch;
}

/*
 * Body of vswitch and clients.
 */

static int stop = 0;

static void *
vswitch_worker(void *opaque)
{
    struct vswitch *p           = opaque;
    struct client *first_client = p->ce->clients + p->first_client;
    unsigned vswitch_idx        = (unsigned int)(p - p->ce->vswitchs);
    unsigned int num_clients    = p->num_clients;
    unsigned int batch          = p->ce->vswitch_batch;
    vswitch_func_t vswitch_func = p->ce->vswitch_func;
    unsigned int i              = 0;
    unsigned long long count    = 0;
    struct timespec t_start, t_end;

    printf("vswitch %u handles %u clients\n", vswitch_idx, num_clients);
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        for (i = 0; i < num_clients; i++) {
            struct client *w = first_client + i;

            count += vswitch_func(w, batch);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    {
        unsigned long long ns =
            1000000000ULL * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);
        double rate = (double)count * 1000.0 / (double)ns;
        printf("vswitch throughput: %.3f Mpps\n", rate);
        p->mpps = rate;
    }

    return NULL;
}

static void *
client_worker(void *opaque)
{
    struct client *c           = opaque;
    client_func_t client_func  = c->ce->client_func;
    size_t iplen               = c->ce->iplen;
    unsigned int first_client  = c->vswitch->first_client;
    unsigned int last_client   = first_client + c->vswitch->num_clients;
    unsigned int dst_idx       = first_client;
    unsigned int client_usleep = c->ce->client_usleep;

    timerslack_reset();

    while (!ACCESS_ONCE(stop)) {
        struct mbuf *m;

        if (client_usleep > 0) {
            usleep(client_usleep);
        }

        /* Allocate and initialize an mbuf. */
        m = mbuf_alloc(iplen, c->idx, dst_idx);
        if (++dst_idx >= last_client) {
            dst_idx = first_client;
        }

        /* Receive arrived mbufs and send the new one. */
        if (client_func(c, m)) {
            mbuf_free(m);
        }
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
           "    [-n NUM_LEAVES = 2]\n"
           "    [-N NUM_ROOTS = 1]\n"
           "    [-l SPSC_QUEUES_LEN = 256]\n"
           "    [-m MBUF_LEN = 256]\n"
           "    [-t QUEUE_TYPE(lq,llq,blq,ffq,iffq,biffq) = lq]\n"
           "    [-b ROOT_BATCH = 8]\n"
           "    [-b LEAF_BATCH = 8]\n"
           "    [-u SENDER_USLEEP = 50]\n",
           progname);
}

int
main(int argc, char **argv)
{
    struct vswitch_experiment _ce;
    struct vswitch_experiment *ce = &_ce;
    size_t memory_size            = 0;
    size_t qsize                  = 0;
    char *memory                  = NULL;
    int opt;
    int ffq; /* boolean */
    int got_b_option = 0;
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
    ce->num_vswitchs  = 1;
    ce->num_clients   = 2;
    ce->qlen          = 128;
    ce->iplen         = 60;
    ce->qtype         = "lq";
    ce->vswitch_batch = 8;
    ce->client_batch  = 1;
    ce->client_usleep = 50;
    ffq               = 0;

    while ((opt = getopt(argc, argv, "hn:l:t:b:N:u:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            ce->num_clients = atoi(optarg);
            if (ce->num_clients == 0 || ce->num_clients > 1000) {
                printf("    Invalid number of analyzers '%s'\n", optarg);
                return -1;
            }
            break;

        case 'N':
            ce->num_vswitchs = atoi(optarg);
            if (ce->num_vswitchs == 0 || ce->num_vswitchs > 1000) {
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

        case 'm':
            ce->iplen = atoi(optarg);
            if (ce->iplen < 60 || ce->iplen > 1500) {
                printf("    Invalid mbuf length '%s'\n", optarg);
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
            if (!got_b_option) {
                got_b_option      = 1;
                ce->vswitch_batch = atoi(optarg);
                if (ce->vswitch_batch < 1 || ce->vswitch_batch > 8192) {
                    printf("    Invalid vswitch batch '%s'\n", optarg);
                    return -1;
                }
            } else {
                ce->client_batch = atoi(optarg);
                if (ce->client_batch < 1 || ce->client_batch > 8192) {
                    printf("    Invalid client batch '%s'\n", optarg);
                    return -1;
                }
            }
            break;

        case 'u':
            ce->client_usleep = atoi(optarg);
            if (ce->client_usleep < 0 || ce->client_usleep > 1000) {
                printf("    Invalid sender usleep argument '%s'\n", optarg);
                return -1;
            }
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    if (ce->num_vswitchs > ce->num_clients) {
        printf("Invalid parameters: num_clients must be "
               ">= num_vswitchs\n");
        return -1;
    }

    /* Subtract the length of the Ethernet header. */
    ce->iplen -= 14;

    qsize = ffq ? iffq_size(ce->qlen) : blq_size(ce->qlen);

    if (!strcmp(ce->qtype, "lq")) {
        ce->vswitch_func = lq_vswitch;
        ce->client_func  = lq_client;
    } else if (!strcmp(ce->qtype, "llq")) {
        ce->vswitch_func = llq_vswitch;
        ce->client_func  = llq_client;
    } else if (!strcmp(ce->qtype, "blq")) {
        ce->vswitch_func = blq_vswitch;
        ce->client_func  = blq_client;
#if 0
    } else if (!strcmp(ce->qtype, "ffq")) {
        ce->vswitch_func = ffq_vswitch;
        ce->client_func  = ffq_client;
    } else if (!strcmp(ce->qtype, "iffq")) {
        ce->vswitch_func = iffq_vswitch;
        ce->client_func  = iffq_client;
    } else if (!strcmp(ce->qtype, "biffq")) {
        ce->vswitch_func = biffq_vswitch;
        ce->client_func  = iffq_client;
#endif
    }

    /*
     * Setup phase.
     */
    {
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;

        memory_size = ce->num_clients * qsize * MAXQ;
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

    ce->clients  = szalloc(ce->num_clients * sizeof(ce->clients[0]));
    ce->vswitchs = szalloc(ce->num_vswitchs * sizeof(ce->vswitchs[0]));

    {
        char *memory_cursor = memory;

        for (i = 0; i < ce->num_clients; i++) {
            struct client *c = ce->clients + i;
            int j;

            c->ce  = ce;
            c->idx = i;
            for (j = 0; j < MAXQ; j++) {
                if (!ffq) {
                    c->blq[j] = (struct Blq *)memory_cursor;
                    blq_init(c->blq[j], ce->qlen);
                } else {
                    c->ffq[j] = (struct Iffq *)memory_cursor;
                    iffq_init(c->ffq[j], ce->qlen, 32 * sizeof(c->ffq[j]->q[0]),
                              /*improved=*/!strcmp(ce->qtype, "iffq"));
                }
                memory_cursor += qsize;
            }
        }
    }

    {
        unsigned int stride =
            (ce->num_clients + ce->num_vswitchs - 1) / ce->num_vswitchs;
        unsigned int overflow    = stride * ce->num_vswitchs - ce->num_clients;
        unsigned int next_client = 0;

        for (i = 0; i < ce->num_vswitchs; i++) {
            struct vswitch *p = ce->vswitchs + i;
            int j;
            p->ce           = ce;
            p->first_client = next_client;
            p->num_clients  = (i < overflow) ? (stride - 1) : stride;
            next_client += p->num_clients;
            for (j = 0; j < p->num_clients; j++) {
                struct client *l = ce->clients + p->first_client + j;
                l->vswitch       = p;
            }
            p->mbufs = szalloc(ce->vswitch_batch * sizeof(p->mbufs[0]));
        }
    }

    for (i = 0; i < ce->num_clients; i++) {
        struct client *w = ce->clients + i;

        if (pthread_create(&w->th, NULL, client_worker, w)) {
            printf("pthread_create(client) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ce->num_vswitchs; i++) {
        struct vswitch *p = ce->vswitchs + i;

        if (pthread_create(&p->th, NULL, vswitch_worker, p)) {
            printf("pthread_create(vswitch) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    printf("Press CTRL-C to stop\n");

    /*
     * Teardown phase.
     */
    for (i = 0; i < ce->num_vswitchs; i++) {
        struct vswitch *p = ce->vswitchs + i;

        if (pthread_join(p->th, NULL)) {
            printf("pthread_join(vswitch) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ce->num_clients; i++) {
        struct client *w = ce->clients + i;

        if (pthread_join(w->th, NULL)) {
            printf("pthread_join(client) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    {
        double tot_mpps = 0.0;

        for (i = 0; i < ce->num_vswitchs; i++) {
            struct vswitch *p = ce->vswitchs + i;

            tot_mpps += p->mpps;
        }

        printf("Total rate %.3f Mpps\n", tot_mpps);
    }

    free(ce->clients);
    free(ce->vswitchs);
    munmap(memory, memory_size);

    return 0;
}
