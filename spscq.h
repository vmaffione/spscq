#ifndef __SPSCQ_H__
#define __SPSCQ_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define compiler_barrier() asm volatile("" ::: "memory")

/* Prepend this to a struct field to make it aligned. */
#define CACHELINE_SIZE 64
#define ALIGN_SIZE 128
#define CACHELINE_ALIGNED __attribute__((aligned(ALIGN_SIZE)))
#define ALIGNED_SIZE(_sz) ((_sz + ALIGN_SIZE - 1) & (~(ALIGN_SIZE - 1)))

#ifdef __cplusplus
}
#endif

#endif /* __SPSCQ_H__ */
