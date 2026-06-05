/** Cache-line aware network model.
 *
 * Divides each cache-line transfer into fixed-size chunks,
 * models serialization delay per chunk over a finite-bandwidth TX link,
 * and returns the resulting round-trip overhead to the coherence controller.
 *
 * getRTT() models:
 *   numChunks       = ceil(cacheLineBytes / chunkBytes)
 *   txBytesPerChunk = chunkBytes + headerBytes
 *   txCyclesPerChunk= ceil(txBytesPerChunk / linkBandwidthBytesPerCycle)
 *   onewayTxLatency = numChunks * txCyclesPerChunk + baseHopDelay
 *   RTT             = 2 * onewayTxLatency
 */

#ifndef CACHELINE_NETWORK_H_
#define CACHELINE_NETWORK_H_

#include <stdint.h>
#include "network.h"
#include "stats.h"

class CachelineNetwork : public Network {
  private:
    // --- configuration (set once in constructor, read-only after) ---
    uint32_t cacheLineBytes;           // size of one cache line in bytes (default 64)
    uint32_t chunkBytes;               // TX payload per chunk in bytes
    uint32_t headerBytes;              // per-chunk header overhead in bytes
    uint32_t linkBandwidthBytesPerCycle; // link bandwidth in bytes/cycle
    uint32_t baseHopDelay;             // fixed routing delay per direction in cycles

    // --- derived (computed once in constructor) ---
    uint32_t numChunks;                // ceil(cacheLineBytes / chunkBytes)
    uint32_t txCyclesPerChunk;         // ceil((chunkBytes + headerBytes) / linkBandwidth)
    uint32_t onewayLatency;            // numChunks * txCyclesPerChunk + baseHopDelay

    // --- stats ---
    Counter statRequests;              // total getRTT() calls
    Counter statChunksSent;            // total chunks transmitted (both directions)
    Counter statTotalTxCycles;         // total TX cycles added across all requests

  public:
    CachelineNetwork(uint32_t _cacheLineBytes,
                     uint32_t _chunkBytes,
                     uint32_t _headerBytes,
                     uint32_t _linkBandwidthBytesPerCycle,
                     uint32_t _baseHopDelay);

    virtual uint32_t getRTT(uint64_t curCycle, uint32_t latency,
                            const char* src, const char* dst) override;

    virtual void initStats(AggregateStat* parent) override;
};

#endif  // CACHELINE_NETWORK_H_
