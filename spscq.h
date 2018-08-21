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

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

/* Support for slot remapping. No slot remapping happens by default. */
#ifndef SMAP
#define SMAP(x) x
#endif

inline int
is_power_of_two(int x)
{
    return !x || !(x & (x - 1));
}

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

inline size_t
blq_size(int qlen)
{
    struct Blq *blq;
    return ALIGNED_SIZE(sizeof(*blq) + qlen * sizeof(blq->q[0]));
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

/*
 * FastForward queue.
 * Many fields are only used by the Improved FastFoward queue (see below).
 */
struct Iffq {
#define IFFQ_PROD_CACHE_ENTRIES 256
    uintptr_t prod_cache[IFFQ_PROD_CACHE_ENTRIES];

    CACHELINE_ALIGNED
    /* Shared (constant) fields. */
    unsigned int entry_mask;
    unsigned int line_entries;
    unsigned int line_mask;

    /* Producer fields. */
    CACHELINE_ALIGNED
    unsigned int prod_write;
    unsigned int prod_check;
    unsigned int prod_cache_write;

    /* Consumer fields. */
    CACHELINE_ALIGNED
    unsigned int cons_clear;
    unsigned int cons_read;

    /* The queue. */
    CACHELINE_ALIGNED
    uintptr_t q[0];
};

inline int
ffq_write(struct Iffq *ffq, uintptr_t m)
{
    uintptr_t *qslot = &ffq->q[SMAP(ffq->prod_write & ffq->entry_mask)];

    if (ACCESS_ONCE(*qslot) != 0) {
        return -1; /* no space */
    }
    ACCESS_ONCE(*qslot) = m;
    ffq->prod_write++;

    return 0;
}

inline uintptr_t
ffq_read(struct Iffq *ffq)
{
    uintptr_t *qslot = &ffq->q[SMAP(ffq->cons_read & ffq->entry_mask)];
    uintptr_t m      = ACCESS_ONCE(*qslot);

    if (m != 0) {
        ACCESS_ONCE(*qslot) = 0; /* clear */
        ffq->cons_read++;
    }

    return m;
}

/*
 * Improved FastForward queue.
 */
inline size_t
iffq_size(unsigned int entries)
{
    struct Iffq *ffq;
    return ALIGNED_SIZE(sizeof(*ffq) + entries * sizeof(ffq->q[0]));
}

/**
 * iffq_init - initialize a pre-allocated mailbox
 * @m: the mailbox to be initialized
 * @entries: the number of entries
 * @line_size: the line size in bytes
 * @improved: 0 for FFQ, 1 for IFFQ
 *
 * Both entries and line_size must be a power of 2.
 * Returns 0 on success, -errno on failure.
 */
int
iffq_init(struct Iffq *ffq, unsigned int entries, unsigned int line_size,
          int improved)
{
    unsigned int entries_per_line;
    unsigned int i;

    if (!is_power_of_two(entries) || !is_power_of_two(line_size) ||
        (improved && entries * sizeof(uintptr_t) <= 2 * line_size) ||
        line_size < sizeof(uintptr_t)) {
        printf("Error: invalid entries/linesize parameters\n");
        return -EINVAL;
    }

    entries_per_line = line_size / sizeof(uintptr_t);

    ffq->line_entries = entries_per_line;
    ffq->line_mask    = ~(entries_per_line - 1);
    ffq->entry_mask   = entries - 1;

    printf("iffq: line_entries %u line_mask %x entry_mask %x\n",
           ffq->line_entries, ffq->line_mask, ffq->entry_mask);

    ffq->cons_clear       = 0;
    ffq->cons_read        = ffq->line_entries;
    ffq->prod_write       = ffq->line_entries;
    ffq->prod_check       = ffq->line_entries;
    ffq->prod_cache_write = 0;

    if (improved) {
        /* For iffq and biffq we need to have something different
         * from nullptr in [cons_clear, cons_read[, or the producer
         * can get confused. */
        for (i = ffq->cons_clear; i != ffq->cons_read; i++) {
            ACCESS_ONCE(ffq->q[SMAP(i)]) = (uintptr_t)1; /* garbage */
        }
    }

    return 0;
}

void
iffq_dump(const char *prefix, struct Iffq *ffq)
{
    printf("[%s]: cc %u, cr %u, pw %u, pc %u\n", prefix, ffq->cons_clear,
           ffq->cons_read, ffq->prod_write, ffq->prod_check);
}

/**
 * iffq_insert - enqueue a new value
 * @ffq: the mailbox where to enqueue
 * @v: the value to be enqueued
 *
 * Returns 0 on success, -ENOBUFS on failure.
 */
inline int
iffq_insert(struct Iffq *ffq, uintptr_t m)
{
    if (unlikely(ffq->prod_write == ffq->prod_check)) {
        /* Leave a cache line empty. */
        if (ACCESS_ONCE(ffq->q[SMAP((ffq->prod_check + ffq->line_entries) &
                                    ffq->entry_mask)]))
            return -ENOBUFS;
        ffq->prod_check += ffq->line_entries;
    }
    ACCESS_ONCE(ffq->q[SMAP(ffq->prod_write & ffq->entry_mask)]) = m;
    ffq->prod_write++;
    return 0;
}

inline unsigned int
iffq_wspace(struct Iffq *ffq)
{
    if (unlikely(ffq->prod_write == ffq->prod_check)) {
        /* Leave a cache line empty. */
        if (ACCESS_ONCE(ffq->q[SMAP((ffq->prod_check + ffq->line_entries) &
                                    ffq->entry_mask)]))
            return 0;
        ffq->prod_check += ffq->line_entries;
    }
    return ffq->prod_check - ffq->prod_write;
}

inline void
iffq_insert_local(struct Iffq *ffq, uintptr_t m)
{
    ffq->prod_cache[ffq->prod_cache_write++] = m;
}

inline void
iffq_insert_publish(struct Iffq *ffq)
{
    unsigned int i;

    for (i = 0; i < ffq->prod_cache_write;
         i++, ffq->prod_write++) {
        ACCESS_ONCE(ffq->q[SMAP(ffq->prod_write & ffq->entry_mask)]) =
            ffq->prod_cache[i];
    }
    ffq->prod_cache_write = 0;
}

/**
 * iffq_extract - extract a value
 * @ffq: the mailbox where to extract from
 *
 * Returns the extracted value, NULL if the mailbox
 * is empty. It does not free up any entry, use
 * iffq_clear for that
 */
inline uintptr_t
iffq_extract(struct Iffq *ffq)
{
    uintptr_t m = ACCESS_ONCE(ffq->q[SMAP(ffq->cons_read & ffq->entry_mask)]);
    if (m) {
        ffq->cons_read++;
    }
    return m;
}

/**
 * iffq_clear - clear the previously extracted entries
 * @ffq: the mailbox to be cleared
 *
 */
inline void
iffq_clear(struct Iffq *ffq)
{
    unsigned int s = (ffq->cons_read - ffq->line_entries) & ffq->line_mask;

    for (; (ffq->cons_clear /* & ffq->line_mask */) != s; ffq->cons_clear++) {
        ACCESS_ONCE(ffq->q[SMAP(ffq->cons_clear & ffq->entry_mask)]) = 0;
    }
}

inline void
iffq_prefetch(struct Iffq *ffq)
{
    __builtin_prefetch((void *)ffq->q[SMAP(ffq->cons_read & ffq->entry_mask)]);
}

#ifdef __cplusplus
}
#endif

#endif /* __SPSCQ_H__ */
