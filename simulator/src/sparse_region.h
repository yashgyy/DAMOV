/**
 * sparse_region.h
 *
 * Sparsity-aware NDP for federated learning: Day 1 infrastructure.
 *
 * Every registered sparse region maintains:
 *   - bitmap[]        : 1 bit per 64-byte cache line
 *                        bit=0 means "line has no nonzero delta"
 *                        bit=1 means "DRAM holds a nonzero delta for this line"
 *   - shadowCurrent[] : byte-level mirror of what the app has written this round
 *   - shadowPrevRound[]: byte-level mirror from the end of the previous round
 *
 * Registration is populated from an mmap hook in zsim.cpp (Day 1 step 4).
 * The NDP core (accelerator_core) will consult these regions in Day 2.
 */

#ifndef SPARSE_REGION_H_
#define SPARSE_REGION_H_

#include <stdint.h>
#include <string.h>
#include <vector>
#include "locks.h"
#include "log.h"

struct SparseRegion {
    uint64_t startAddr;
    uint64_t endAddr;
    uint32_t numLines;

    std::vector<uint64_t> bitmap;
    std::vector<uint8_t>  shadowCurrent;
    std::vector<uint8_t>  shadowPrevRound;

    SparseRegion(uint64_t start, uint64_t size)
        : startAddr(start), endAddr(start + size), numLines((uint32_t)(size / 64)) {
        bitmap.resize((numLines + 63) / 64, 0);
        shadowCurrent.resize((size_t)numLines * 64, 0);
        shadowPrevRound.resize((size_t)numLines * 64, 0);
    }
};

/* -------------------------------------------------------------------------
 * Global registry (defined in sparse_region.cpp)
 * ------------------------------------------------------------------------- */
extern std::vector<SparseRegion*> g_sparseRegions;
extern lock_t g_sparseLock;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Register a new region. Called from mmap hook. Idempotent. */
void registerSparseRegion(uint64_t addr, uint64_t size);

/* Look up a region by address. Returns nullptr if addr is not in any region.
 * Fast path used on every memory access — no locking (see impl comment). */
SparseRegion* findSparseRegion(uint64_t addr);

/* Called by the PIN store observer whenever the app writes to memory.
 * Copies data into shadowCurrent of the matching region (if any). */
void observeSparseStore(uint64_t addr, uint32_t size, const uint8_t* data);

/* Called at FL round boundary (from HandleMagicOp on ROI_END).
 * Snapshots shadowCurrent -> shadowPrevRound, clears bitmaps. */
void advanceSparseRound();

#endif  // SPARSE_REGION_H_
