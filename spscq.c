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

struct lampq {
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

    /* The queue. */
    CACHELINE_ALIGNED
    struct mbuf *q[0];
};

static struct lampq *
lampq_create(int qlen)
{
    struct lampq *lq =
        szalloc(sizeof(struct lampq) + qlen * sizeof(struct mbuf));

    lq->qlen = qlen;

    return lq;
}

int
main(int argc, char **argv)
{
    {
        struct lampq *lq = lampq_create(128);
        free(lq);
    }
    return 0;
}
