#ifndef PHASE_HINT_H
#define PHASE_HINT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "zsim_hooks.h"

enum { FL_IDLE = 0, FL_INGRESS = 1, FL_TRAIN = 2, FL_EGRESS = 3 };

/* PhaSched hints: phase register (1040) and NIC DMA model (1041-1043) */
#define ZSIM_MAGIC_OP_PHASE (1040)   /* rdx = phase */

static inline void zsim_op2(uint64_t op, uint64_t a, uint64_t b) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "d"(a), "S"(b));
    COMPILER_BARRIER();
}
#ifdef PHASCHED_NO_HINTS
static inline void fl_phase(int p) { (void)p; }
#else
static inline void fl_phase(int p) { zsim_op2(ZSIM_MAGIC_OP_PHASE, (uint64_t)p, 0); }
#endif
#define ZSIM_MAGIC_OP_DMA_WRITE (1041)   /* NIC -> DRAM : rdx = addr, rsi = bytes */
#define ZSIM_MAGIC_OP_DMA_READ  (1042)   /* DRAM -> NIC : rdx = addr, rsi = bytes */
#define ZSIM_MAGIC_OP_DMA_POLL  (1043)   /* rdx <- 1 while the NIC is busy */

static inline uint64_t zsim_op_ret(uint64_t op) {
    uint64_t r = 0;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : "+d"(r) : "c"(op));
    COMPILER_BARRIER();
    return r;
}
#if defined(PHASCHED_NO_HINTS) || defined(PHASCHED_NO_DMA)
/* CPU proxy for the NIC */
static inline void fl_dma_in(void* b, size_t n)  { memset(b, 1, n); }
static inline void fl_dma_out(void* b, size_t n) {
    volatile float s = 0; const float* f = (const float*)b;
    for (size_t i = 0; i < n / sizeof(float); i += 16) s += f[i];
}
#else
static inline void fl_dma_wait(void) { while (zsim_op_ret(ZSIM_MAGIC_OP_DMA_POLL)) __builtin_ia32_pause(); }
static inline void fl_dma_in (void* b, size_t n) { zsim_op2(ZSIM_MAGIC_OP_DMA_WRITE, (uint64_t)b, n); fl_dma_wait(); }
static inline void fl_dma_out(void* b, size_t n) { zsim_op2(ZSIM_MAGIC_OP_DMA_READ,  (uint64_t)b, n); fl_dma_wait(); }
#endif
#endif
