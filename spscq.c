#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static unsigned int
msq_wspace(struct msq *mq)
{
    return (mq->read - 1 - mq->write_priv) & mq->qmask;
}

/* No boundary checks, to be called after msq_wspace(). */
static void
msq_write_local(struct msq *mq, struct mbuf *m)
{
    mq->q[mq->write_priv & mq->qmask] = m;
    mq->write_priv++;
}

static void
msq_write_publish(struct msq *mq)
{
    mq->write = mq->write_priv;
}

static int
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

static unsigned int
msq_rspace(struct msq *mq)
{
    return mq->write - mq->read_priv;
}

/* No boundary checks, to be called after msq_rspace(). */
static struct mbuf *
msq_read_local(struct msq *mq)
{
    struct mbuf *m = mq->q[mq->read_priv];
    mq->read_priv++;
    return m;
}

static void
msq_read_publish(struct msq *mq)
{
    mq->read = mq->read_priv;
}

static struct mbuf *
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

int
main(int argc, char **argv)
{
    {
        struct msq *mq = msq_create(128, 4);
        struct mbuf *m = mbuf_alloc();

        printf("wspace %u rspace %u\n", msq_wspace(mq), msq_rspace(mq));
        m->len = 18;
        msq_write(mq, m);
        printf("wspace %u rspace %u\n", msq_wspace(mq), msq_rspace(mq));
        m = msq_read(mq);
        printf("wspace %u rspace %u\n", msq_wspace(mq), msq_rspace(mq));
        free(mq);
        free(m);
    }

    return 0;
}
