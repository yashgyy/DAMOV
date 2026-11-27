// OCCARB Step 4 unit tests for ramulator/NicModel.h (no simulator needed).
// Build+run from simulator/:  bash occarb/step4/run_check.sh
#include <cstdio>
#include <cmath>
#include "NicModel.h"
using namespace ramulator;

static int fails = 0;
static void check(bool ok, const char* name, const char* detail = "") {
    printf("%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) fails++;
}
static char buf[256];
#define D(...) (snprintf(buf, sizeof buf, __VA_ARGS__), buf)

// Drain helper: complete every wanted DMA immediately (memory infinitely fast).
static void drain_all(NicModel& n) {
    while (n.want_rx_write()) n.complete_rx_write(n.issue_rx_write());
    while (n.want_tx_read()) { n.issue_tx_read(); n.complete_tx_read(); }
}

int main() {
    // 4a: link-rate arithmetic. 10 Gb/s at 1.2 GHz = 1.25e9 B/s / 1.2e9 = 1.0417 B/cycle.
    {
        NicParams p; p.link_gbps = 10; p.cycle_hz = 1.2e9; p.tx_enable = false; p.buf_bytes = 1e9;
        NicModel n(p);
        check(std::fabs(n.bytes_per_cycle_link() - 1.25e9 / 1.2e9) < 1e-9, "4a bytes/cycle at 10GbE, 1.2GHz",
              D("(%.6f)", n.bytes_per_cycle_link()));
        for (int i = 0; i < 1200000; i++) n.tick();          // 1 ms
        double expect = 1.25e6;                               // 1.25 MB in 1 ms
        check(std::fabs(n.rx_bytes_accepted - expect) <= p.pkt_bytes, "4a 1 ms at 10GbE delivers 1.25 MB (+-1 pkt)",
              D("(%.0f B)", n.rx_bytes_accepted));
        NicParams p1 = p; p1.link_gbps = 1;  NicModel n1(p1);
        NicParams p25 = p; p25.link_gbps = 25; NicModel n25(p25);
        for (int i = 0; i < 1200000; i++) { n1.tick(); n25.tick(); }
        check(std::fabs(n1.rx_bytes_accepted - 1.25e5) <= p.pkt_bytes && std::fabs(n25.rx_bytes_accepted - 3.125e6) <= p.pkt_bytes,
              "4a 1GbE and 25GbE scale linearly", D("(%.0f B, %.0f B)", n1.rx_bytes_accepted, n25.rx_bytes_accepted));
    }
    // 4b: overflow when memory never drains -> drops, buffer never exceeds S.
    {
        NicParams p; p.buf_bytes = 64 * 1024; p.tx_enable = false;
        NicModel n(p);
        for (int i = 0; i < 1000000; i++) n.tick();
        check(n.rx_pkts_dropped > 0 && n.rx_fill <= p.buf_bytes, "4b no drain -> packet loss, fill <= S",
              D("(dropped=%llu, fill=%.0f)", (unsigned long long)n.rx_pkts_dropped, n.rx_fill));
        check(n.rx_loss_rate() > 0.9, "4b loss rate -> ~1 once full", D("(%.3f)", n.rx_loss_rate()));
    }
    // 4c: zero loss when memory is faster than the link.
    {
        NicParams p; p.buf_bytes = 64 * 1024; p.tx_enable = false;
        NicModel n(p);
        for (int i = 0; i < 1000000; i++) { n.tick(); drain_all(n); }
        check(n.rx_pkts_dropped == 0, "4c fast drain -> zero loss");
        check(std::fabs(n.rx_bytes_written - n.rx_bytes_accepted) < p.line_bytes, "4c all accepted bytes reach memory",
              D("(acc=%.0f wr=%.0f)", n.rx_bytes_accepted, n.rx_bytes_written));
    }
    // 4d: memory slower than the link (0.64 B/cycle < 1.04 B/cycle) -> loss (paper's loss-avoidance condition).
    {
        NicParams p; p.buf_bytes = 64 * 1024; p.tx_enable = false;
        NicModel n(p);
        for (int i = 0; i < 1000000; i++) {
            n.tick();
            if (i % 100 == 0 && n.want_rx_write()) n.complete_rx_write(n.issue_rx_write());   // 64 B / 100 cycles
        }
        check(n.rx_pkts_dropped > 0, "4d DRAM bw < link rate -> loss", D("(loss=%.3f)", n.rx_loss_rate()));
    }
    // 4e: 2-bit quantisation boundaries. S = 4 packets, no drain: fill = k/4 after k packets.
    {
        NicParams p; p.pkt_bytes = 1500; p.buf_bytes = 4 * 1500.0; p.tx_enable = false; p.link_gbps = 10;
        NicModel n(p);
        int expect[] = {0, 1, 2, 3, 3};
        bool ok = (n.level() == 0);
        for (int k = 1; k <= 4; k++) {
            while (n.rx_pkts_arrived < (uint64_t)k) n.tick();
            ok = ok && n.level() == expect[k] && std::fabs(n.occupancy() - k / 4.0) < 1e-12;
        }
        check(ok, "4e level = floor(4*o), clamped to 3 (o = 0, .25, .5, .75, 1)");
        NicParams p3 = p; p3.occ_bits = 3; NicModel n3(p3);
        while (n3.rx_pkts_arrived < 2) n3.tick();              // o = 0.5 -> level 4 of 0..7
        check(n3.level() == 4, "4e occ_bits=3 -> 8 levels", D("(level=%d)", n3.level()));
    }
    // 4f: overlap ratio rho.
    {
        double rhos[] = {0.0, 0.5, 1.0};
        for (double r : rhos) {
            NicParams p; p.rho = r; p.period_cycles = 10000; p.buf_bytes = 1e9;
            NicModel n(p);
            for (int i = 0; i < 1000000; i++) n.tick();
            double frac = (double)n.active_cycles / n.cycle;
            check(std::fabs(frac - r) < 1e-3, D("4f rho=%.1f -> active fraction", r), "");
            printf("      active fraction = %.4f, RX bytes = %.0f\n", frac, n.rx_bytes_accepted);
            if (r == 0.0) check(n.rx_pkts_arrived == 0 && n.tx_reads_issued == 0, "4f rho=0 -> no traffic at all");
        }
    }
    // 4g: TX: empty buffer while active -> urgency 1 and underruns; fed buffer -> link-rate goodput.
    {
        NicParams p; p.rx_enable = false; p.buf_bytes = 64 * 1024;
        NicModel n(p);
        n.tick();
        check(n.occupancy() == 1.0 && n.level() == 3, "4g active + empty TX -> o = 1 (URGENT range)");
        for (int i = 0; i < 100000; i++) n.tick();
        check(n.tx_underruns > 0 && n.tx_pkts_sent == 0, "4g no DMA reads -> link underruns");
        NicModel m(p);
        for (int i = 0; i < 1200000; i++) { m.tick(); drain_all(m); }
        check(std::fabs(m.tx_bytes_sent - 1.25e6) <= 2 * p.pkt_bytes, "4g fed TX sends 1.25 MB/ms at 10GbE",
              D("(%.0f B)", m.tx_bytes_sent));
        check(m.occupancy() < 0.05, "4g full TX buffer -> low urgency", D("(o=%.4f)", m.occupancy()));
        NicParams pi = p; pi.rho = 0.0; NicModel idle(pi); idle.tick();
        check(idle.occupancy() == 0.0, "4g idle link -> TX emptiness ignored");
    }
    // 4h: o = max(RX fill, TX emptiness).
    {
        NicParams p; p.buf_bytes = 4 * 1500.0;
        NicModel n(p);
        while (n.rx_pkts_arrived < 1) n.tick();                // RX 0.25 full, TX empty+active -> 1.0
        check(n.occupancy() == std::max(n.rx_occupancy(), n.tx_emptiness()) && n.occupancy() == 1.0,
              "4h o = max(rx_fill, tx_emptiness)");
    }
    // 4i: conservation: arrived = accepted + dropped; accepted = written + fill.
    {
        NicParams p; p.buf_bytes = 32 * 1024; p.tx_enable = false;
        NicModel n(p);
        for (int i = 0; i < 500000; i++) { n.tick(); if (i % 70 == 0 && n.want_rx_write()) n.complete_rx_write(n.issue_rx_write()); }
        bool ok1 = std::fabs(n.rx_pkts_arrived * (double)p.pkt_bytes - (n.rx_bytes_accepted + n.rx_pkts_dropped * (double)p.pkt_bytes)) < 1e-6;
        bool ok2 = std::fabs(n.rx_bytes_accepted - (n.rx_bytes_written + n.rx_fill)) < 1e-6;
        check(ok1 && ok2, "4i byte conservation (arrived = accepted + dropped; accepted = written + buffered)");
    }
    // 4j: in-flight requests are not re-issued, and the tail is flushed when the link goes idle.
    {
        NicParams p; p.tx_enable = false; p.rho = 0.5; p.period_cycles = 20000; p.buf_bytes = 1e6;
        NicModel n(p);
        while (n.rx_pkts_arrived < 1) n.tick();                // 1500 B buffered = 23 full lines + 28 B
        int issued = 0; uint32_t sizes = 0;
        while (n.want_rx_write()) { sizes += n.issue_rx_write(); issued++; }
        check(issued == 23 && sizes == 23 * 64, "4j only full lines issued while active", D("(%d reqs)", issued));
        while (n.active()) n.tick();                           // run to the idle half
        while (n.want_rx_write()) { sizes += n.issue_rx_write(); issued++; }
        check(sizes == (uint32_t)n.rx_fill, "4j idle -> partial tail flushed", D("(%u of %.0f B)", sizes, n.rx_fill));
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL NIC TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
