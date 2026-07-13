/**
 * sparse_region.cpp
 *
 * Implementation of the global sparse-region registry declared in
 * sparse_region.h. See that header for design commentary.
 */

#include "sparse_region.h"

std::vector<SparseRegion*> g_sparseRegions;
lock_t g_sparseLock;

/* Initialize the lock on first use. C++ static-init handles this safely
 * because both the vector and the futex are POD-ish initialized. We call
 * futex_init explicitly from the mmap hook the first time it fires. */
static bool s_sparseLockInited = false;

static inline void lazyInitLock() {
    if (!s_sparseLockInited) {
        futex_init(&g_sparseLock);
        s_sparseLockInited = true;
    }
}

void registerSparseRegion(uint64_t addr, uint64_t size) {
    lazyInitLock();
    futex_lock(&g_sparseLock);

    /* Idempotent: skip duplicates so a re-mmap doesn't create a second region
     * on top of the first (which would leak the shadow memory). */
    for (auto* r : g_sparseRegions) {
        if (r->startAddr == addr) {
            futex_unlock(&g_sparseLock);
            return;
        }
    }

    g_sparseRegions.push_back(new SparseRegion(addr, size));
    info("[sparsity] Registered region: 0x%lx (%lu KB, %u lines)",
         addr, size / 1024, (uint32_t)(size / 64));

    futex_unlock(&g_sparseLock);
}

SparseRegion* findSparseRegion(uint64_t addr) {
    /* No lock on the hot path.
     *
     * Rationale: mmap registrations only happen at workload startup (or very
     * rarely mid-run). Reads from g_sparseRegions on the hot path (every
     * memory access) far outnumber writes. Worst case if a race happens: we
     * miss a region for one access — no corruption, only a lost stat.
     *
     * If this becomes a real problem, swap for a seqlock or RCU pattern. */
    for (auto* r : g_sparseRegions) {
        if (addr >= r->startAddr && addr < r->endAddr) return r;
    }
    return nullptr;
}

void observeSparseStore(uint64_t addr, uint32_t size, const uint8_t* data) {
    SparseRegion* r = findSparseRegion(addr);
    if (!r) return;
    uint64_t offset = addr - r->startAddr;
    /* Safety: don't overrun the shadow buffer. */
    if (offset + size > r->shadowCurrent.size()) return;
    memcpy(&r->shadowCurrent[offset], data, size);
}

void advanceSparseRound() {
    lazyInitLock();
    futex_lock(&g_sparseLock);
    for (auto* r : g_sparseRegions) {
        memcpy(r->shadowPrevRound.data(),
               r->shadowCurrent.data(),
               r->shadowCurrent.size());
        std::fill(r->bitmap.begin(), r->bitmap.end(), 0);
    }
    futex_unlock(&g_sparseLock);
}
