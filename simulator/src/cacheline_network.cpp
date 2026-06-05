#include "cacheline_network.h"
#include "log.h"
#include <cmath>

CachelineNetwork::CachelineNetwork(uint32_t _cacheLineBytes,
                                   uint32_t _chunkBytes,
                                   uint32_t _headerBytes,
                                   uint32_t _linkBandwidthBytesPerCycle,
                                   uint32_t _baseHopDelay)
    : cacheLineBytes(_cacheLineBytes),
      chunkBytes(_chunkBytes),
      headerBytes(_headerBytes),
      linkBandwidthBytesPerCycle(_linkBandwidthBytesPerCycle),
      baseHopDelay(_baseHopDelay)
{
    if (chunkBytes == 0)
        panic("CachelineNetwork: chunkBytes must be > 0");
    if (linkBandwidthBytesPerCycle == 0)
        panic("CachelineNetwork: linkBandwidthBytesPerCycle must be > 0");

    // How many chunks does one cache line split into?
    numChunks = (cacheLineBytes + chunkBytes - 1) / chunkBytes;

    // How many cycles to transmit one chunk over the TX link?
    uint32_t totalBytesPerChunk = chunkBytes + headerBytes;
    txCyclesPerChunk = (totalBytesPerChunk + linkBandwidthBytesPerCycle - 1)
                        / linkBandwidthBytesPerCycle;

    // One-way latency: routing delay + serialization of all chunks
    onewayLatency = baseHopDelay + numChunks * txCyclesPerChunk;

    info("[CachelineNetwork] cacheLineBytes=%u chunkBytes=%u headerBytes=%u "
         "linkBW=%u B/cyc baseHopDelay=%u",
         cacheLineBytes, chunkBytes, headerBytes,
         linkBandwidthBytesPerCycle, baseHopDelay);
    info("[CachelineNetwork] numChunks=%u txCyclesPerChunk=%u onewayLatency=%u RTT=%u",
         numChunks, txCyclesPerChunk, onewayLatency, 2 * onewayLatency);
}

uint32_t CachelineNetwork::getRTT(uint64_t curCycle, uint32_t latency,
                                  const char* src, const char* dst) {
    // Update stats
    statRequests.inc();
    statChunksSent.inc(numChunks * 2);          // request + response both send chunks
    statTotalTxCycles.inc(2 * onewayLatency);   // both directions

    // Return the round-trip overhead added on top of the underlying cache latency.
    // Coherence controller adds this to respCycle:
    //   respCycle += nextLevelLat + getRTT()
    return 2 * onewayLatency;
}

void CachelineNetwork::initStats(AggregateStat* parent) {
    AggregateStat* netStat = new AggregateStat();
    netStat->init("cachelineNet", "CachelineNetwork stats");

    statRequests.init("requests", "Total network requests (getRTT calls)");
    statChunksSent.init("chunksSent", "Total chunks transmitted (both directions)");
    statTotalTxCycles.init("totalTxCycles", "Total TX cycles added to requests");

    netStat->append(&statRequests);
    netStat->append(&statChunksSent);
    netStat->append(&statTotalTxCycles);

    parent->append(netStat);
}
