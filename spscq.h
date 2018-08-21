#ifndef __SPSCQ_H__
#define __SPSCQ_H__

#ifdef __cplusplus
extern "C" {
#define ACCESS_ONCE(x)                                                         \
    (*static_cast<std::remove_reference<decltype(x)>::type volatile *>(&(x)))
#else
#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))
#endif

#include <stdint.h>
#include <stdio.h>

#define compiler_barrier() asm volatile("" ::: "memory")

/* Prepend this to a struct field to make it aligned. */
#define CACHELINE_SIZE 64
#define ALIGN_SIZE 128
#define CACHELINE_ALIGNED __attribute__((aligned(ALIGN_SIZE)))
#define ALIGNED_SIZE(_sz) ((_sz + ALIGN_SIZE - 1) & (~(ALIGN_SIZE - 1)))

#ifndef SMAP
#define SMAP(x) x
#endif

/*
 * Multi-section queue, based on the Lamport classic queue.
 * All indices are free running.
 */
struct Blq {
    /* Producer private data. */
    CACHELINE_ALIGNED
    unsigned int write_priv;
    unsigned int read_shadow;

    /* Producer write, consumer read. */
    CACHELINE_ALIGNED
    unsigned int write;

    /* Consumer private data. */
    CACHELINE_ALIGNED
    unsigned int read_priv;
    unsigned int write_shadow;

    /* Producer read, consumer write. */
    CACHELINE_ALIGNED
    unsigned int read;

    /* Shared read only data. */
    CACHELINE_ALIGNED
    unsigned int qlen;
    unsigned int qmask;

    /* The queue. */
    CACHELINE_ALIGNED
    uintptr_t q[0];
};

inline int
is_power_of_two(int x)
{
    return !x || !(x & (x - 1));
}

inline int
blq_init(struct Blq *blq, int qlen)
{
    if (qlen < 2 || !is_power_of_two(qlen)) {
        printf("Error: queue length %d is not a power of two\n", qlen);
        return -1;
    }

    blq->qlen  = qlen;
    blq->qmask = qlen - 1;

    return 0;
}

inline int
lq_write(struct Blq *q, uintptr_t m)
{
    unsigned write    = q->write;
    unsigned int next = (write + 1) & q->qmask;

    if (next == ACCESS_ONCE(q->read)) {
        return -1; /* no space */
    }
    ACCESS_ONCE(q->q[SMAP(write)]) = m;
    compiler_barrier();
    ACCESS_ONCE(q->write) = next;
    return 0;
}

inline uintptr_t
lq_read(struct Blq *q)
{
    unsigned read = q->read;
    uintptr_t m;

    if (read == ACCESS_ONCE(q->write)) {
        return 0; /* queue empty */
    }
    compiler_barrier();
    m                    = ACCESS_ONCE(q->q[SMAP(read)]);
    ACCESS_ONCE(q->read) = (read + 1) & q->qmask;
    return m;
}

inline int
llq_write(struct Blq *q, uintptr_t m)
{
    unsigned int write = q->write;
    unsigned int check =
        (write + (CACHELINE_SIZE / sizeof(uintptr_t))) & q->qmask;

    if (check == q->read_shadow) {
        q->read_shadow = ACCESS_ONCE(q->read);
    }
    if (check == q->read_shadow) {
        return -1; /* no space */
    }
    ACCESS_ONCE(q->q[SMAP(write)]) = m;
    compiler_barrier();
    ACCESS_ONCE(q->write) = (write + 1) & q->qmask;
    return 0;
}

inline uintptr_t
llq_read(struct Blq *q)
{
    unsigned read = q->read_priv;
    uintptr_t m;
    if (read == q->write_shadow) {
        q->write_shadow = ACCESS_ONCE(q->write);
        if (read == q->write_shadow) {
            return 0; /* queue empty */
        }
    }
    compiler_barrier();
    m                    = ACCESS_ONCE(q->q[SMAP(read)]);
    ACCESS_ONCE(q->read) = q->read_priv = (read + 1) & q->qmask;
    return m;
}

inline unsigned int
blq_wspace(struct Blq *blq)
{
    unsigned int space =
        (blq->read_shadow - (CACHELINE_SIZE / sizeof(uintptr_t)) -
         blq->write_priv) &
        blq->qmask;

    if (space) {
        return space;
    }
    blq->read_shadow = ACCESS_ONCE(blq->read);

    return (blq->read_shadow - (CACHELINE_SIZE / sizeof(uintptr_t)) -
            blq->write_priv) &
           blq->qmask;
}

/* No boundary checks, to be called after blq_wspace(). */
inline void
blq_write_local(struct Blq *blq, uintptr_t m)
{
    ACCESS_ONCE(blq->q[SMAP(blq->write_priv & blq->qmask)]) = m;
    blq->write_priv++;
}

inline void
blq_write_publish(struct Blq *blq)
{
    /* Here we need a StoreStore barrier to prevent previous stores to the
     * queue slot and mbuf content to be reordered after the store to
     * blq->write. On x86 a compiler barrier suffices, because stores have
     * release semantic (preventing StoreStore and LoadStore reordering). */
    compiler_barrier();
    ACCESS_ONCE(blq->write) = blq->write_priv;
}

inline unsigned int
blq_rspace(struct Blq *blq)
{
    unsigned int space = blq->write_shadow - blq->read_priv;

    if (space) {
        return space;
    }
    blq->write_shadow = ACCESS_ONCE(blq->write);
    /* Here we need a LoadLoad barrier to prevent upcoming loads to the queue
     * slot and mbuf content to be reordered before the load of blq->write. On
     * x86 a compiler barrier suffices, because loads have acquire semantic
     * (preventing LoadLoad and LoadStore reordering). */
    compiler_barrier();

    return blq->write_shadow - blq->read_priv;
}

/* No boundary checks, to be called after blq_rspace(). */
inline uintptr_t
blq_read_local(struct Blq *blq)
{
    uintptr_t m = ACCESS_ONCE(blq->q[SMAP(blq->read_priv & blq->qmask)]);
    blq->read_priv++;
    return m;
}

inline void
blq_read_publish(struct Blq *blq)
{
    ACCESS_ONCE(blq->read) = blq->read_priv;
}

inline void
blq_dump(const char *prefix, struct Blq *blq)
{
    printf("[%s] r %u rspace %u w %u wspace %u\n", prefix,
           blq->read & blq->qmask, blq_rspace(blq), blq->write & blq->qmask,
           blq_wspace(blq));
}

#ifdef __cplusplus
}
#endif

#endif /* __SPSCQ_H__ */
