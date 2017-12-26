#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

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

/* Multi-section queue, based on the Lamport classic queue.
 * All indices are free running. */
struct msq {
    /* Producer private data. */
    CACHELINE_ALIGNED
    unsigned int write_priv;

    /* Producer write, consumer read. */
    CACHELINE_ALIGNED
    unsigned int write;

    /* Consumer private data. */
    CACHELINE_ALIGNED
    unsigned int read_priv;

    /* Producer read, consumer write. */
    CACHELINE_ALIGNED
    unsigned int read;

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
    unsigned int write_next = mq->write_priv + 1;

    if (write_next == mq->read) {
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
    struct mbuf *m = mq->q[mq->read_priv];
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

struct global {
    unsigned int qlen;
    unsigned int rbatch;
    struct msq *mq;
};

static void *
producer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    struct msq *mq   = g->mq;
    struct mbuf *m   = mbuf_alloc();

    msq_dump("P", mq);
    msq_write(mq, m);
    msq_dump("P", mq);

    pthread_exit(NULL);
    return NULL;
}

static void *
consumer(void *opaque)
{
    struct global *g = (struct global *)opaque;
    struct msq *mq   = g->mq;
    struct mbuf *m;

    msq_dump("C", mq);
    m = msq_read(mq);
    msq_dump("C", mq);

    pthread_exit(NULL);
    return NULL;
}

int
main(int argc, char **argv)
{
    pthread_t pth, cth;
    struct global _g;
    struct global *g = &_g;

    {
        g->qlen   = 128;
        g->rbatch = 4;
        g->mq     = msq_create(g->qlen, g->rbatch);

        if (pthread_create(&pth, NULL, producer, g)) {
            perror("pthread_create(producer");
            exit(EXIT_FAILURE);
        }

        if (pthread_create(&cth, NULL, consumer, g)) {
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

        free(g->mq);
    }

    return 0;
}
