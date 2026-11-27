// OCCARB: NIC buffer model (paper §II-A, §III "Network model").
//
// Header-only and dependency-free, so it can be unit-tested standalone (occarb/step4) and
// included both by ZSim (injector, Step 5) and Ramulator (scheduler, Steps 6-7).
//
// Model, advanced one cycle at a time by tick():
//  * The link is active for a fraction rho of every `period_cycles` window (overlap ratio).
//  * RX (download, e.g. a new global model): while active, packets of `pkt_bytes` arrive at the
//    link rate. A packet that does not fit in the free RX space is dropped (= packet loss).
//    Buffered bytes are drained by DMA *writes* to memory (cache-bypassing, one line each);
//    space is freed when the write completes.
//  * TX (upload, e.g. a model update): the NIC fetches data with DMA *reads* while active;
//    fetched bytes fill the TX buffer, the link drains it at the link rate. If a packet is due
//    but not fully buffered, the link idles (underrun).
//  * Occupancy (urgency) o = max(RX fill, TX emptiness), each as a fraction of the buffer.
//    TX emptiness counts only while the link is active (an idle link cannot underrun).
//    level() quantises o to `occ_bits` bits (paper: 2 bits).
#ifndef __OCCARB_NIC_MODEL_H
#define __OCCARB_NIC_MODEL_H

#include <cstdint>
#include <algorithm>

namespace ramulator {

struct NicParams {
    double   buf_bytes     = 1 << 20;   // S: size of each of the RX and TX buffers (bytes)
    double   link_gbps     = 10.0;      // link rate per direction (Gbit/s)
    double   cycle_hz      = 1.2e9;     // frequency at which tick() is called
    double   rho           = 1.0;       // overlap ratio: fraction of each period the link is active
    uint64_t period_cycles = 1000000;   // length of one active/idle window
    uint32_t pkt_bytes     = 1500;      // link packet size (MTU)
    uint32_t line_bytes    = 64;        // DMA request size (one cache line)
    bool     rx_enable     = true;
    bool     tx_enable     = true;
    int      occ_bits      = 2;         // quantisation of the exported level
};

class NicModel {
public:
    explicit NicModel(const NicParams& p) : p(p) {
        bytes_per_cycle = p.link_gbps * 1e9 / 8.0 / p.cycle_hz;
        active_len = (uint64_t)(p.rho * (double)p.period_cycles + 0.5);
    }

    // ---- time ----
    // Advance one cycle: link arrivals (RX) and link departures (TX).
    void tick() {
        is_active = (cycle % p.period_cycles) < active_len;
        cycle++;
        if (!is_active) return;
        active_cycles++;
        credit += bytes_per_cycle;
        while (credit >= p.pkt_bytes) {                 // one packet time has elapsed
            credit -= p.pkt_bytes;
            if (p.rx_enable) {
                rx_pkts_arrived++;
                if (rx_fill + p.pkt_bytes <= p.buf_bytes) { rx_fill += p.pkt_bytes; rx_bytes_accepted += p.pkt_bytes; }
                else                                      { rx_pkts_dropped++; }
            }
            if (p.tx_enable) {
                if (tx_fill >= p.pkt_bytes) { tx_fill -= p.pkt_bytes; tx_pkts_sent++; tx_bytes_sent += p.pkt_bytes; }
                else                        { tx_underruns++; }
            }
        }
    }

    // ---- RX: DMA writes NIC -> memory ----
    bool want_rx_write() const {
        if (!p.rx_enable) return false;
        double pending = rx_fill - rx_inflight;          // buffered but not yet issued
        return pending >= p.line_bytes || (pending > 0 && !is_active);  // flush tail when idle
    }
    uint32_t issue_rx_write() {                          // returns bytes covered by this request
        double pending = rx_fill - rx_inflight;
        uint32_t b = (uint32_t)std::min<double>(p.line_bytes, pending);
        rx_inflight += b; rx_writes_issued++;
        return b;
    }
    void complete_rx_write(uint32_t b) { rx_inflight -= b; rx_fill -= b; rx_bytes_written += b; }

    // ---- TX: DMA reads memory -> NIC ----
    bool want_tx_read() const {
        return p.tx_enable && is_active && tx_fill + tx_inflight + p.line_bytes <= p.buf_bytes;
    }
    void issue_tx_read()    { tx_inflight += p.line_bytes; tx_reads_issued++; }
    void complete_tx_read() { tx_inflight -= p.line_bytes; tx_fill += p.line_bytes; tx_bytes_read += p.line_bytes; }

    // ---- occupancy signal ----
    double rx_occupancy() const { return p.rx_enable ? rx_fill / p.buf_bytes : 0.0; }
    double tx_emptiness() const { return (p.tx_enable && is_active) ? 1.0 - tx_fill / p.buf_bytes : 0.0; }
    double occupancy()    const { return std::max(rx_occupancy(), tx_emptiness()); }
    int level() const {
        int levels = 1 << p.occ_bits;
        int l = (int)(occupancy() * levels);
        return std::min(std::max(l, 0), levels - 1);
    }
    bool active() const { return is_active; }

    // ---- derived metrics ----
    double rx_loss_rate() const { return rx_pkts_arrived ? (double)rx_pkts_dropped / rx_pkts_arrived : 0.0; }
    double bytes_per_cycle_link() const { return bytes_per_cycle; }

    // ---- state / stats (public for reporting and tests) ----
    NicParams p;
    double   bytes_per_cycle = 0, credit = 0;
    uint64_t active_len = 0, cycle = 0, active_cycles = 0;
    bool     is_active = false;
    double   rx_fill = 0, rx_inflight = 0, tx_fill = 0, tx_inflight = 0;
    uint64_t rx_pkts_arrived = 0, rx_pkts_dropped = 0, rx_writes_issued = 0;
    uint64_t tx_pkts_sent = 0, tx_underruns = 0, tx_reads_issued = 0;
    double   rx_bytes_accepted = 0, rx_bytes_written = 0, tx_bytes_read = 0, tx_bytes_sent = 0;
};

} // namespace ramulator
#endif
