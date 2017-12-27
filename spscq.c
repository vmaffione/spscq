#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "tsc.h"

#undef QDEBUG

#define CACHELINE_ALIGNED __attribute__((aligned(64)))

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

struct mbuf {
    unsigned int len;
    unsigned int __padding[7];
#define MBUF_LEN_MAX 1500
    char buf[MBUF_LEN_MAX];
};

struct mbuf *
mbuf_alloc()
{
    return szalloc(sizeof(struct mbuf));
}

void
mbuf_free(struct mbuf *m)
{
    if (m) {
        free(m);
    }
}

/* Multi-section queue, based on the Lamport classic queue.
 * All indices are free running. */
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
    unsigned int rbatch;

    /* The queue. */
    CACHELINE_ALIGNED
    struct mbuf *q[0];
};

static struct msq *
msq_create(int qlen, int rbatch)
{
    struct msq *mq = szalloc(sizeof(*mq) + qlen * sizeof(mq->q[0]));

    if (qlen < 2 || ((qlen & (qlen - 1)) != 0)) {
        exit(EXIT_FAILURE);
    }

    mq->qlen   = qlen;
    mq->qmask  = qlen - 1;
    mq->rbatch = rbatch;

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
    struct mbuf *m;

    while ((m = msq_read(mq)) != NULL) {
        mbuf_free(m);
    }

    memset(mq, 0, sizeof(*mq));
    free(mq);
}

struct global {
    /* Test length as a number of packets. */
    long int num_packets;

    /* Queue length. */
    unsigned int qlen;

    /* Max consumer batch. */
    unsigned int rbatch;

    struct timespec begin, end;

    /* The queue. */
    struct msq *mq;
};

static void *
msq_producer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    long int left    = g->num_packets;
    struct mbuf *m   = mbuf_alloc();
    struct msq *mq   = g->mq;

    clock_gettime(CLOCK_MONOTONIC, &g->begin);
    while (left > 0) {
#ifdef QDEBUG
        msq_dump("P", mq);
#endif
        if (msq_write(mq, m) == 0) {
            --left;
        }
    }
    msq_dump("P", mq);

    pthread_exit(NULL);
    return NULL;
}

static void *
msq_consumer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    long int left    = g->num_packets;
    struct msq *mq   = g->mq;
    struct mbuf *m;

    while (left > 0) {
#ifdef QDEBUG
        msq_dump("C", mq);
#endif
        m = msq_read(mq);
        if (m) {
            --left;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &g->end);
    msq_dump("C", mq);

    pthread_exit(NULL);
    return NULL;
}

static void
usage(const char *progname)
{
    printf("%s [-h]"
           " [-n NUM_PACKETS]"
           " [-b MAX_CONSUMER_BATCH]"
           " [-l QUEUE_LENGTH]"
           "\n",
           progname);
}

int
main(int argc, char **argv)
{
    pthread_t pth, cth;
    struct global _g;
    struct global *g = &_g;
    int opt;

    memset(g, 0, sizeof(*g));
    g->num_packets = 10000000;
    g->qlen        = 128;
    g->rbatch      = 4;

    while ((opt = getopt(argc, argv, "hn:b:l:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;

        case 'n':
            g->num_packets = atoi(optarg);
            if (g->num_packets < 0) {
                printf("    Invalid number of packets '%s'\n", optarg);
                return -1;
            }
            break;

        case 'b':
            g->rbatch = atoi(optarg);
            if (g->rbatch < 1) {
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
        }
    }

    tsc_init();

    {
        unsigned long int ndiff;
        double mpps;

        g->mq = msq_create(g->qlen, g->rbatch);

        if (pthread_create(&pth, NULL, msq_producer, g)) {
            perror("pthread_create(msq_producer");
            exit(EXIT_FAILURE);
        }

        if (pthread_create(&cth, NULL, msq_consumer, g)) {
            perror("pthread_create(msq_consumer)");
            exit(EXIT_FAILURE);
        }

        if (pthread_join(pth, NULL)) {
            perror("pthread_join(msq_producer)");
            exit(EXIT_FAILURE);
        }

        if (pthread_join(cth, NULL)) {
            perror("pthread_join(msq_consumer)");
            exit(EXIT_FAILURE);
        }

        ndiff = (g->end.tv_sec - g->begin.tv_sec) * 1000000000U +
                (g->end.tv_nsec - g->begin.tv_nsec);
        mpps = g->num_packets * 1000.0 / ndiff;
        printf("Throughput %3.3f Mpps\n", mpps);

        msq_free(g->mq);
    }

    return 0;
}
