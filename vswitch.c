/*
 * Copyright (C) 2018 Universita' di Pisa
 * Copyright (C) 2018 Vincenzo Maffione
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

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
#include <sys/sysinfo.h>

#include "mlib.h"
#include "spscq.h"

#define DEBUG
#undef DEBUG

struct mbuf {
    struct mbuf *next;
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
    m->next    = NULL;
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

struct vswitch_list {
    struct mbuf *head;
    struct mbuf *tail;
    unsigned int len;
};

/* Context of a vswitch thread (load balancer or transmission). */
struct vswitch {
    struct vswitch_experiment *ce;
    pthread_t th;
    int cpu;
    struct client *clients;
    unsigned int first_client;
    unsigned int last_client;
    unsigned int num_clients;
    struct vswitch_list *lists;
    double mpps;
};

/* Context of a client thread (traffic analyzer or sender clients). */
struct client {
    struct vswitch_experiment *ce;
    struct vswitch *vswitch;
    pthread_t th;
    int cpu;
    unsigned int idx;
    struct mbuf **mbufs;
#define TXQ 0
#define RXQ 1
#define MAXQ 2
    struct Blq *blq[MAXQ];
    struct Iffq *ffq[MAXQ];
};

typedef unsigned int (*vswitch_pull_t)(struct client *c, unsigned int batch);
typedef void (*vswitch_push_t)(struct client *c, struct mbuf *m,
                               unsigned int num_bufs);
typedef int (*client_func_t)(struct client *c, unsigned int batch);

struct vswitch_experiment {
    /* Type of spsc queue to be used. */
    const char *qtype;

    /* Number of (vswitch) threads performing load balancing or transmission. */
    unsigned int num_vswitches;

    /* Number of (client) threads performing traffic analysis or sender clients.
     */
    unsigned int num_clients;

    /* Length of each SPSC queue. */
    unsigned int qlen;

    /* Boolean: latency experiment rather than throughput? */
    int latency;

    /* Test duration in seconds. */
    int duration;

    /* Boolean: should we pin threads to cores?
     * If yes, should we avoid using some CPUs? */
    int pin_threads;
#define MAX_RESERVED_CPUS 16
    int reserved_cpus[MAX_RESERVED_CPUS];
    unsigned int num_reserved_cpus;

    /* Length of each mbuf IP payload. */
    unsigned iplen;

    /* Batch size (in packets) for vswitch and client operation. */
    unsigned int vswitch_batch;
    unsigned int client_batch;

    /* Vswitch work. */
    vswitch_pull_t vswitch_pull_func;
    vswitch_push_t vswitch_push_func;

    /* Client work. */
    client_func_t client_func_a;
    client_func_t client_func_b;

    /* Client nodes. */
    struct client *clients;

    /* Vswitch nodes. */
    struct vswitch *vswitches;
    struct vswitch_list *lists;

    /* Microseconds for sender usleep(). */
    unsigned int client_usleep;
};

static inline uint16_t
mbuf_dst(struct mbuf *m)
{
    uint16_t *dst_ptr = (uint16_t *)&m->dst_mac[4];
    return ntohs(*dst_ptr);
}

static inline void
mbuf_dst_set(struct mbuf *m, uint16_t dst_idx)
{
    uint16_t *dst_ptr = (uint16_t *)&m->dst_mac[4];
    *dst_ptr          = htons(dst_idx);
}

static inline uint16_t
mbuf_src(struct mbuf *m)
{
    uint16_t *src_ptr = (uint16_t *)&m->src_mac[4];
    return ntohs(*src_ptr);
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

static inline void
mbuf_list_append(struct vswitch *v, struct mbuf *m)
{
    struct vswitch_list *list;

    list = v->lists + mbuf_dst(m);
    if (list->tail) {
        list->tail->next = m;
        list->tail       = m;
    } else {
        list->tail = list->head = m;
    }
    list->len++;
}

static int stop = 0;

/*
 * Queue-specific client and vswitch implementations.
 */

static int
lq_client(struct client *c, unsigned int batch)
{
    struct mbuf *m;
    unsigned int i;

    while ((m = (struct mbuf *)lq_read(c->blq[RXQ])) != NULL) {
        mbuf_free(m);
    }

    for (i = 0; i < batch; i++) {
        if (lq_write(c->blq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
    }

    return i;
}

static int
lq_rr_client(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        if (lq_write(c->blq[TXQ], (uintptr_t)c->mbufs[i])) {
            return i;
        }
    }

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)lq_read(c->blq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return batch;
            }
        }
        mbuf_free(m);
    }

    return batch;
}

static int
lq_rr_server(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)lq_read(c->blq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return i;
            }
        }
        mbuf_dst_set(c->mbufs[i], mbuf_src(m));
        if (lq_write(c->blq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
        mbuf_free(m);
    }

    return i;
}

static unsigned int
lq_vswitch_pull(struct client *c, unsigned int batch)
{
    struct vswitch *v = c->vswitch;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = (struct mbuf *)lq_read(c->blq[TXQ]);

        if (m == NULL) {
            break;
        }
        mbuf_list_append(v, m);
    }

    return count;
}

static void
lq_vswitch_push(struct client *c, struct mbuf *m, unsigned int num_bufs)
{
    do {
        struct mbuf *next = m->next;

        if (lq_write(c->blq[RXQ], (uintptr_t)m)) {
            mbuf_free(m);
        }
        m = next;
    } while (m != NULL);
}

static int
llq_client(struct client *c, unsigned int batch)
{
    struct mbuf *m;
    unsigned int i;

    while ((m = (struct mbuf *)llq_read(c->blq[RXQ])) != NULL) {
        mbuf_free(m);
    }

    for (i = 0; i < batch; i++) {
        if (llq_write(c->blq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
    }

    return i;
}

static int
llq_rr_client(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        if (llq_write(c->blq[TXQ], (uintptr_t)c->mbufs[i])) {
            return i;
        }
    }

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)llq_read(c->blq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return batch;
            }
        }
        mbuf_free(m);
    }

    return batch;
}

static int
llq_rr_server(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)llq_read(c->blq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return i;
            }
        }
        mbuf_dst_set(c->mbufs[i], mbuf_src(m));
        if (llq_write(c->blq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
        mbuf_free(m);
    }

    return i;
}

static unsigned int
llq_vswitch_pull(struct client *c, unsigned int batch)
{
    struct vswitch *v = c->vswitch;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = (struct mbuf *)llq_read(c->blq[TXQ]);

        if (m == NULL) {
            break;
        }
        mbuf_list_append(v, m);
    }

    return count;
}

static void
llq_vswitch_push(struct client *c, struct mbuf *m, unsigned int num_bufs)
{
    do {
        struct mbuf *next = m->next;

        if (llq_write(c->blq[RXQ], (uintptr_t)m)) {
            mbuf_free(m);
        }
        m = next;
    } while (m != NULL);
}

static int
blq_client(struct client *c, unsigned int batch)
{
    unsigned int space = blq_rspace(c->blq[RXQ], batch);
    unsigned int i;

    for (; space > 0; space--) {
        struct mbuf *m = (struct mbuf *)blq_read_local(c->blq[RXQ]);
        mbuf_free(m);
    }
    blq_read_publish(c->blq[RXQ]);

    space = blq_wspace(c->blq[TXQ], batch);
    if (batch > space) {
        batch = space;
    }
    for (i = 0; i < batch; i++) {
        blq_write_local(c->blq[TXQ], (uintptr_t)c->mbufs[i]);
    }
    blq_write_publish(c->blq[TXQ]);

    return i;
}

static int
blq_rr_client(struct client *c, unsigned int batch)
{
    unsigned int space = blq_wspace(c->blq[TXQ], batch);
    unsigned int i;

    if (batch > space) {
        batch = space;
    }
    for (i = 0; i < batch; i++) {
        blq_write_local(c->blq[TXQ], (uintptr_t)c->mbufs[i]);
    }
    blq_write_publish(c->blq[TXQ]);

    for (space = 0, i = 0; i < batch; space--, i++) {
        struct mbuf *m;

        while (space == 0) {
            if (unlikely(ACCESS_ONCE(stop))) {
                blq_read_publish(c->blq[RXQ]);
                return batch;
            }
            space = blq_rspace(c->blq[RXQ], batch);
        }
        m = (struct mbuf *)blq_read_local(c->blq[RXQ]);
        mbuf_free(m);
    }
    blq_read_publish(c->blq[RXQ]);

    return batch;
}

static int
blq_rr_server(struct client *c, unsigned int batch)
{
    unsigned int space = blq_rspace(c->blq[RXQ], batch);
    unsigned int i;

    if (batch > space) {
        batch = space;
    }
    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        m = (struct mbuf *)blq_read_local(c->blq[RXQ]);
        mbuf_dst_set(c->mbufs[i], mbuf_src(m));
        mbuf_free(m);
    }
    blq_read_publish(c->blq[RXQ]);

    space = blq_wspace(c->blq[TXQ], batch);
    if (batch > space) {
        batch = space;
    }
    for (i = 0; i < batch; i++) {
        blq_write_local(c->blq[TXQ], (uintptr_t)c->mbufs[i]);
    }
    blq_write_publish(c->blq[TXQ]);

    return batch;
}

static unsigned int
blq_vswitch_pull(struct client *c, unsigned int batch)
{
    unsigned int rspace = blq_rspace(c->blq[TXQ], batch);
    struct vswitch *v   = c->vswitch;
    unsigned int count;

    if (batch > rspace) {
        batch = rspace;
    }

    for (count = 0; count < batch; count++) {
        struct mbuf *m = (struct mbuf *)blq_read_local(c->blq[TXQ]);
        mbuf_list_append(v, m);
    }
    blq_read_publish(c->blq[TXQ]);

    return count;
}

static void
blq_vswitch_push(struct client *c, struct mbuf *m, unsigned int num_bufs)
{
    struct Blq *blq     = c->blq[RXQ];
    unsigned int wspace = blq_wspace(blq, num_bufs);

    do {
        struct mbuf *next = m->next;

        if (wspace > 0) {
            blq_write_local(blq, (uintptr_t)m);
            wspace--;
        } else {
            mbuf_free(m);
        }
        m = next;
    } while (m != NULL);

    blq_write_publish(blq);
}

static int
ffq_client(struct client *c, unsigned int batch)
{
    struct mbuf *m;
    unsigned int i;

    while ((m = (struct mbuf *)ffq_read(c->ffq[RXQ])) != NULL) {
        mbuf_free(m);
    }

    for (i = 0; i < batch; i++) {
        if (ffq_write(c->ffq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
    }

    return i;
}

static int
ffq_rr_client(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        if (ffq_write(c->ffq[TXQ], (uintptr_t)c->mbufs[i])) {
            return i;
        }
    }

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)ffq_read(c->ffq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return batch;
            }
        }
        mbuf_free(m);
    }

    return batch;
}

static int
ffq_rr_server(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)ffq_read(c->ffq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return i;
            }
        }
        mbuf_dst_set(c->mbufs[i], mbuf_src(m));
        if (ffq_write(c->ffq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
        mbuf_free(m);
    }

    return i;
}

static unsigned int
ffq_vswitch_pull(struct client *c, unsigned int batch)
{
    struct vswitch *v = c->vswitch;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = (struct mbuf *)ffq_read(c->ffq[TXQ]);

        if (m == NULL) {
            break;
        }
        mbuf_list_append(v, m);
    }

    return count;
}

static void
ffq_vswitch_push(struct client *c, struct mbuf *m, unsigned int num_bufs)
{
    do {
        struct mbuf *next = m->next;

        if (ffq_write(c->ffq[RXQ], (uintptr_t)m)) {
            mbuf_free(m);
        }
        m = next;
    } while (m != NULL);
}

static int
iffq_client(struct client *c, unsigned int batch)
{
    struct mbuf *m;
    unsigned int i;

    while ((m = (struct mbuf *)iffq_extract(c->ffq[RXQ])) != NULL) {
        mbuf_free(m);
    }
    iffq_clear(c->ffq[RXQ]);

    for (i = 0; i < batch; i++) {
        if (iffq_insert(c->ffq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
    }

    return i;
}

static int
iffq_rr_client(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        if (iffq_insert(c->ffq[TXQ], (uintptr_t)c->mbufs[i])) {
            return i;
        }
    }

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)iffq_extract(c->ffq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return batch;
            }
        }
        mbuf_free(m);
    }
    iffq_clear(c->ffq[RXQ]);

    return batch;
}

static int
iffq_rr_server(struct client *c, unsigned int batch)
{
    unsigned int i;

    for (i = 0; i < batch; i++) {
        struct mbuf *m;

        while ((m = (struct mbuf *)iffq_extract(c->ffq[RXQ])) == NULL) {
            if (unlikely(ACCESS_ONCE(stop))) {
                return i;
            }
        }
        mbuf_dst_set(c->mbufs[i], mbuf_src(m));
        if (iffq_insert(c->ffq[TXQ], (uintptr_t)c->mbufs[i])) {
            break;
        }
        mbuf_free(m);
    }
    iffq_clear(c->ffq[RXQ]);

    return i;
}

static unsigned int
iffq_vswitch_pull(struct client *c, unsigned int batch)
{
    struct vswitch *v = c->vswitch;
    unsigned int count;

    for (count = 0; count < batch; count++) {
        struct mbuf *m = (struct mbuf *)iffq_extract(c->ffq[TXQ]);

        if (m == NULL) {
            break;
        }
        mbuf_list_append(v, m);
    }
    iffq_clear(c->ffq[TXQ]);

    return count;
}

static void
iffq_vswitch_push(struct client *c, struct mbuf *m, unsigned int num_bufs)
{
    do {
        struct mbuf *next = m->next;

        if (iffq_insert(c->ffq[RXQ], (uintptr_t)m)) {
            mbuf_free(m);
        }
        m = next;
    } while (m != NULL);
}

static int
biffq_client(struct client *c, unsigned int batch)
{
    unsigned int wspace;
    struct mbuf *m;
    unsigned int i;

    while ((m = (struct mbuf *)iffq_extract(c->ffq[RXQ])) != NULL) {
        mbuf_free(m);
    }
    iffq_clear(c->ffq[RXQ]);

    wspace = iffq_wspace(c->ffq[TXQ], batch);
    if (batch > wspace) {
        batch = wspace;
    }
    for (i = 0; i < batch; i++) {
        iffq_insert_local(c->ffq[TXQ], (uintptr_t)c->mbufs[i]);
    }
    iffq_insert_publish(c->ffq[TXQ]);

    return batch;
}

static void
biffq_vswitch_push(struct client *c, struct mbuf *m, unsigned int num_bufs)
{
    struct Iffq *ffq    = c->ffq[RXQ];
    unsigned int wspace = iffq_wspace(ffq, num_bufs);

    do {
        struct mbuf *next = m->next;

        if (wspace > 0) {
            iffq_insert_local(ffq, (uintptr_t)m);
            wspace--;
        } else {
            mbuf_free(m);
        }
        m = next;
    } while (m != NULL);

    iffq_insert_publish(ffq);
}

/*
 * Body of vswitch and clients.
 */

static void *
vswitch_worker(void *opaque)
{
    struct vswitch *v           = opaque;
    struct client *first_client = v->clients;
    unsigned vswitch_idx        = (unsigned int)(v - v->ce->vswitches);
    unsigned int num_clients    = v->num_clients;
    unsigned int batch          = v->ce->vswitch_batch;
    vswitch_pull_t vswitch_pull = v->ce->vswitch_pull_func;
    vswitch_push_t vswitch_push = v->ce->vswitch_push_func;
    unsigned int i              = 0;
    unsigned long long count    = 0;
    struct timespec t_start, t_end;

    printf("vswitch %u handles %u clients\n", vswitch_idx, num_clients);
    if (v->cpu >= 0) {
        runon("vswitch", v->cpu);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        /* Pull packets from all the clients. */
        for (i = 0; i < num_clients; i++) {
            struct client *c = first_client + i;

            count += vswitch_pull(c, batch);
        }
#ifdef DEBUG
        for (i = 0; i < num_clients; i++) {
            struct vswitch_list *list = v->lists + i;
            struct mbuf *m            = list->head;

            while (m != NULL) {
                printf("send %u --> %u\n", mbuf_src(m), i);
                m = m->next;
            }
        }
#endif
        /* Push packets to all the clients. */
        for (i = 0; i < num_clients; i++) {
            struct vswitch_list *list = v->lists + i;
            struct client *c          = v->clients + i;
            struct mbuf *m            = list->head;

            if (m) {
                vswitch_push(c, m, list->len);
                list->head = list->tail = NULL;
                list->len               = 0;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    {
        unsigned long long ns =
            1000000000ULL * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);
        double rate = (double)count * 1000.0 / (double)ns;
        printf("vswitch throughput: %.3f Mpps\n", rate);
        v->mpps = rate;
    }

    return NULL;
}

static void *
client_worker(void *opaque)
{
    struct client *c           = opaque;
    client_func_t client_func  = NULL;
    size_t iplen               = c->ce->iplen;
    unsigned int num_clients   = c->vswitch->num_clients;
    unsigned int client_usleep = c->ce->client_usleep;
    unsigned int batch         = c->ce->client_batch;
    unsigned int dst_idx       = c->idx & (~0x1); /* phase */
    unsigned int dst_idx_inc   = c->ce->latency ? 2 : 1;
    unsigned int consumed      = batch;

    if (!c->ce->latency || (c->idx % 2) == 1) {
        client_func = c->ce->client_func_a;
    } else {
        client_func = c->ce->client_func_b;
    }

    c->mbufs = szalloc(batch * sizeof(c->mbufs[0]));

    if (c->cpu >= 0) {
        runon("client", c->cpu);
    }

    timerslack_reset();

    while (!ACCESS_ONCE(stop)) {
        unsigned int i;

        if (client_usleep > 0) {
            usleep(client_usleep);
        }

        /* Allocate and initialize a pool of mbufs. */
        for (i = 0; i < consumed; i++) {
            c->mbufs[i] = mbuf_alloc(iplen, c->idx, dst_idx);
            dst_idx += dst_idx_inc;
            if (dst_idx >= num_clients) {
                dst_idx = 0;
            }
        }

        /* Receive arrived mbufs and send the new ones. */
        consumed = i = client_func(c, batch);

        /* Reuse all the ones that were not transmitted. */
        for (; i < batch; i++) {
            mbuf_dst_set(c->mbufs[i], dst_idx);
            dst_idx += dst_idx_inc;
            if (dst_idx >= num_clients) {
                dst_idx = 0;
            }
        }
    }

    return NULL;
}

static void
malloc_benchmark(struct vswitch_experiment *ce)
{
    struct timespec t_start, t_end;
    unsigned long long count = 0;

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    while (!ACCESS_ONCE(stop)) {
        struct mbuf *m = mbuf_alloc(ce->iplen, 1, 3);
        mbuf_free(m);
        count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    {
        unsigned long long ns =
            1000000000ULL * (t_end.tv_sec - t_start.tv_sec) +
            (t_end.tv_nsec - t_start.tv_nsec);
        double rate = (double)count * 1000.0 / (double)ns;
        printf("Malloc benchmark: %.3f Mpps\n", rate);
    }
}

static int
cpu_is_reserved(struct vswitch_experiment *ce, int cpu)
{
    int i;

    for (i = 0; i < ce->num_reserved_cpus; i++) {
        if (ce->reserved_cpus[i] == cpu) {
            return 1;
        }
    }

    return 0;
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
           "    [-D DURATION_SECONDS = 10]\n"
           "    [-n NUM_VSWITCHES = 1]\n"
           "    [-N NUM_CLIENTS = 2]\n"
           "    [-l SPSC_QUEUES_LEN = 128]\n"
           "    [-m MBUF_LEN = 60]\n"
           "    [-t QUEUE_TYPE(lq,llq,blq,ffq,iffq,biffq) = lq]\n"
           "    [-b VSWITCH_BATCH = 8]\n"
           "    [-b CLIENT_BATCH = 1]\n"
           "    [-u SENDER_USLEEP = 0]\n"
           "    [-T (run latency workload)]\n"
           "    [-p (pin theads to cores)]\n"
           "    [-x CPU_TO_LEAVE_UNUSED]\n"
           "    [-j (run leaf benchmark)]\n",
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
    int ffq;              /* boolean */
    int benchmark    = 0; /* boolean */
    int got_b_option = 0;
    int ncpus        = get_nprocs();
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
    ce->duration          = 10;
    ce->num_vswitches     = 1;
    ce->num_clients       = 2;
    ce->qlen              = 128;
    ce->iplen             = 60 - sizeof(struct mbuf *);
    ce->qtype             = "lq";
    ce->vswitch_batch     = 8;
    ce->client_batch      = 1;
    ce->client_usleep     = 0;
    ce->pin_threads       = 0;
    ce->num_reserved_cpus = 0;
    ce->latency           = 0;
    ffq                   = 0;

    while ((opt = getopt(argc, argv, "hn:l:t:b:N:u:px:TjD:")) != -1) {
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
            ce->num_vswitches = atoi(optarg);
            if (ce->num_vswitches == 0 || ce->num_vswitches > 1000) {
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

        case 'p':
            ce->pin_threads = 1;
            break;

        case 'x': {
            int cpu;

            cpu = atoi(optarg);
            if (cpu < 0 || cpu >= ncpus) {
                printf("    Invalid CPU id '%s'\n", optarg);
                return -1;
            }
            if (ce->num_reserved_cpus >= MAX_RESERVED_CPUS) {
                printf("    Too many reserved CPUs\n");
                return -1;
            }
            ce->reserved_cpus[ce->num_reserved_cpus++] = cpu;
            break;
        }

        case 'T':
            ce->latency = 1;
            break;

        case 'j':
            benchmark = 1;
            break;

        case 'D':
            ce->duration = atoi(optarg);
            if (ce->duration == 0 || ce->duration > 1000) {
                printf("    Invalid duration '%s'\n", optarg);
                return -1;
            }
            break;

        default:
            usage(argv[0]);
            return 0;
            break;
        }
    }

    /* Subtract the length of the Ethernet header. */
    ce->iplen -= 14;

    if (benchmark) {
        printf("Running malloc benchmark: CTRL-C to stop\n");
        malloc_benchmark(ce);
        return 0;
    }

    if (ce->num_vswitches > ce->num_clients) {
        printf("Invalid parameters: num_clients must be "
               ">= num_vswitches\n");
        return -1;
    }

    if (ncpus - (int)ce->num_vswitches < (int)ce->num_reserved_cpus) {
        printf("Infeasible CPU assignment\n");
        return -1;
    }

    qsize = ffq ? iffq_size(ce->qlen) : blq_size(ce->qlen);

    if (!strcmp(ce->qtype, "lq")) {
        ce->vswitch_pull_func = lq_vswitch_pull;
        ce->vswitch_push_func = lq_vswitch_push;
        ce->client_func_a     = ce->latency ? lq_rr_client : lq_client;
        ce->client_func_b     = lq_rr_server;
    } else if (!strcmp(ce->qtype, "llq")) {
        ce->vswitch_pull_func = llq_vswitch_pull;
        ce->vswitch_push_func = llq_vswitch_push;
        ce->client_func_a     = ce->latency ? llq_rr_client : llq_client;
        ce->client_func_b     = llq_rr_server;
    } else if (!strcmp(ce->qtype, "blq")) {
        ce->vswitch_pull_func = blq_vswitch_pull;
        ce->vswitch_push_func = blq_vswitch_push;
        ce->client_func_a     = ce->latency ? blq_rr_client : blq_client;
        ce->client_func_b     = blq_rr_server;
    } else if (!strcmp(ce->qtype, "ffq")) {
        ce->vswitch_pull_func = ffq_vswitch_pull;
        ce->vswitch_push_func = ffq_vswitch_push;
        ce->client_func_a     = ce->latency ? ffq_rr_client : ffq_client;
        ce->client_func_b     = ffq_rr_server;
    } else if (!strcmp(ce->qtype, "iffq")) {
        ce->vswitch_pull_func = iffq_vswitch_pull;
        ce->vswitch_push_func = iffq_vswitch_push;
        ce->client_func_a     = ce->latency ? iffq_rr_client : iffq_client;
        ce->client_func_b     = iffq_rr_server;
    } else if (!strcmp(ce->qtype, "biffq")) {
        ce->vswitch_pull_func = iffq_vswitch_pull;
        ce->vswitch_push_func = biffq_vswitch_push;
        ce->client_func_a     = ce->latency ? iffq_rr_client : biffq_client;
        ce->client_func_b     = iffq_rr_server;
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

    ce->clients   = szalloc(ce->num_clients * sizeof(ce->clients[0]));
    ce->lists     = szalloc(ce->num_clients * sizeof(ce->lists[0]));
    ce->vswitches = szalloc(ce->num_vswitches * sizeof(ce->vswitches[0]));

    {
        char *memory_cursor = memory;
        int cpu_next        = (ce->num_vswitches % ncpus) - 1;

        for (i = 0; i < ce->num_clients; i++) {
            struct client *c = ce->clients + i;
            int j;

            do {
                if (++cpu_next >= ncpus) {
                    cpu_next = ce->num_vswitches % ncpus;
                }
            } while (cpu_is_reserved(ce, cpu_next));

            c->ce    = ce;
            c->cpu   = ce->pin_threads ? cpu_next : -1;
            c->mbufs = NULL;
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
            (ce->num_clients + ce->num_vswitches - 1) / ce->num_vswitches;
        unsigned int overflow    = stride * ce->num_vswitches - ce->num_clients;
        unsigned int next_client = 0;

        for (i = 0; i < ce->num_vswitches; i++) {
            struct vswitch *v = ce->vswitches + i;
            int j;
            v->ce           = ce;
            v->cpu          = ce->pin_threads ? (i % ncpus) : -1;
            v->first_client = next_client;
            v->num_clients  = (i < overflow) ? (stride - 1) : stride;
            v->last_client  = v->first_client + v->num_clients;
            v->clients      = ce->clients + v->first_client;
            v->lists        = ce->lists + v->first_client;
            next_client += v->num_clients;
            for (j = 0; j < v->num_clients; j++) {
                struct client *c = ce->clients + v->first_client + j;
                c->vswitch       = v;
                c->idx           = j;
            }
            if (ce->latency && v->num_clients % 2 != 0) {
                printf("Latency experiment requires an even number "
                       "of clients for each vswitch\n");
                return -1;
            }
        }
    }

    for (i = 0; i < ce->num_clients; i++) {
        struct client *w = ce->clients + i;

        if (pthread_create(&w->th, NULL, client_worker, w)) {
            printf("pthread_create(client) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < ce->num_vswitches; i++) {
        struct vswitch *p = ce->vswitches + i;

        if (pthread_create(&p->th, NULL, vswitch_worker, p)) {
            printf("pthread_create(vswitch) failed\n");
            exit(EXIT_FAILURE);
        }
    }

    printf("Press CTRL-C to stop\n");

    sleep(ce->duration);
    ACCESS_ONCE(stop) = 1;

    /*
     * Teardown phase.
     */
    for (i = 0; i < ce->num_vswitches; i++) {
        struct vswitch *p = ce->vswitches + i;

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

        for (i = 0; i < ce->num_vswitches; i++) {
            struct vswitch *p = ce->vswitches + i;

            tot_mpps += p->mpps;
        }

        printf("Total rate %.3f Mpps\n", tot_mpps);
    }

    free(ce->clients);
    free(ce->vswitches);
    munmap(memory, memory_size);

    return 0;
}
