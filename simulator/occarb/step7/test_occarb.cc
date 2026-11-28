// OCCARB Step 7 unit tests: occupancy-driven mode switching in the Ramulator controller.
// Drives Ramulator through RamulatorWrapper (the interface ZSim uses), no ZSim/Pin.
// Usage: test_occarb <base DDR4 cfg> <scratch dir>     (built and run by occarb/step7/run_check.sh)
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <functional>
#include "RamulatorWrapper.h"
#include "Request.h"
#include "Memory.h"
#include "DDR4.h"
using namespace ramulator;

static int fails = 0;
static void check(bool ok, const std::string& name, const std::string& detail = "") {
    printf("%s %s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (!ok) fails++;
}
static std::string fmt(const char* f, double a, double b) { char buf[200]; snprintf(buf, sizeof buf, f, a, b); return buf; }

struct Req { long addr; bool dma; bool write; Req(long a, bool d, bool w) : addr(a), dma(d), write(w) {} };
struct Result { std::vector<long> done_at; long ticks; long transitions; long modes[3]; long blocked; };

// <base cfg> without src_policy/occarb_*/relaxed_strict lines + the given extra lines
static int cfg_id = 0;
static std::string make_cfg(const std::string& base, const std::string& dir, const std::vector<std::string>& extra) {
    std::string path = dir + "/occarb_" + std::to_string(cfg_id++) + ".cfg";
    std::ifstream in(base); std::ofstream out(path);
    std::string line;
    while (std::getline(in, line))
        if (line.find("src_policy") == std::string::npos && line.find("occarb_") == std::string::npos &&
            line.find("relaxed_strict") == std::string::npos) out << line << "\n";
    for (auto& e : extra) out << " " << e << "\n";
    return path;
}
static Controller<DDR4>* ctrl_of(RamulatorWrapper& w) { return dynamic_cast<Memory<DDR4>*>(w.mem)->ctrls[0]; }

// Ask Ramulator how it maps an address: (channel, rank, bankgroup, bank, row)
static std::vector<int> decode(const std::string& cfg, long addr) {
    RamulatorWrapper w(cfg.c_str(), 1, 64, false, false, "occarb_step7_decode", false);
    Request q(addr, Request::Type::READ, [](Request&) {}, 0);
    w.send(q);
    auto v = ctrl_of(w)->readq.q.back().addr_vec;
    return std::vector<int>(v.begin(), v.begin() + int(DDR4::Level::Row) + 1);
}
static bool same_bank(const std::vector<int>& a, const std::vector<int>& b) {
    for (int l = 0; l <= int(DDR4::Level::Bank); l++) if (a[l] != b[l]) return false;
    return true;
}

static Result run(const std::string& cfg, const std::vector<Req>& reqs) {
    RamulatorWrapper w(cfg.c_str(), 1, 64, false, false, "occarb_step7_test", false);
    Result r; r.done_at.assign(reqs.size(), -1);
    long now = 0; size_t done = 0;
    for (size_t i = 0; i < reqs.size(); i++) {
        auto cb = [&, i](Request&) { r.done_at[i] = now; done++; };
        Request q(reqs[i].addr, reqs[i].write ? Request::Type::WRITE : Request::Type::READ, cb, 0);
        q.is_dma = reqs[i].dma;
        while (!w.send(q)) { w.tick(); now++; }
    }
    while (done < reqs.size() && now < 10000000) { w.tick(); now++; }
    for (int k = 0; k < 50; k++) { w.tick(); now++; }      // let the mode settle after the last completion
    auto* c = ctrl_of(w);
    r.ticks = now; r.transitions = c->occ_transitions; r.blocked = c->strict_blocked;
    for (int m = 0; m < 3; m++) r.modes[m] = c->occ_mode_cycles[m];
    return r;
}
static double mean_lat(const Result& r, const std::vector<Req>& reqs, bool dma) {
    double s = 0; int n = 0;
    for (size_t i = 0; i < reqs.size(); i++) if (reqs[i].dma == dma) { s += r.done_at[i]; n++; }
    return n ? s / n : 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <base cfg> <scratch dir>\n", argv[0]); return 2; }
    std::string base = argv[1], dir = argv[2];

    // ---- 7a: state machine (Table I / Fig. 1b), 2-bit levels, theta_lo = 0.25, theta_hi = 0.75 ----
    {
        std::string cfg = make_cfg(base, dir, {"src_policy = OCCARB", "occarb_theta_lo = 0.25", "occarb_theta_hi = 0.75"});
        RamulatorWrapper w(cfg.c_str(), 1, 64, false, false, "occarb_step7_fsm", false);
        auto* c = ctrl_of(w);
        typedef Controller<DDR4>::OccMode M;
        struct Stepx { int level; M expect; const char* why; };
        std::vector<Stepx> seq = {
            {0, M::RELAXED, "o=0    -> RELAXED"},
            {1, M::NEUTRAL, "o=.25  -> NEUTRAL (o >= lo)"},
            {2, M::NEUTRAL, "o=.50  -> NEUTRAL"},
            {3, M::URGENT,  "o=.75  -> URGENT (o >= hi)"},
            {2, M::URGENT,  "o=.50  -> stays URGENT (hysteresis)"},
            {1, M::URGENT,  "o=.25  -> stays URGENT (not < lo)"},
            {0, M::RELAXED, "o=0    -> RELAXED (o < lo leaves URGENT)"},
            {3, M::URGENT,  "o=.75  -> URGENT directly from RELAXED"},
            {0, M::RELAXED, "o=0    -> RELAXED"},
            {2, M::NEUTRAL, "o=.50  -> NEUTRAL"},
            {1, M::NEUTRAL, "o=.25  -> stays NEUTRAL"},
            {0, M::RELAXED, "o=0    -> RELAXED (o < lo leaves NEUTRAL)"}};
        bool ok = true; std::string bad;
        for (auto& st : seq) {
            w.set_io_level(st.level, 4);
            w.tick();
            if (c->occ_mode != st.expect) { ok = false; bad += std::string(" [") + st.why + "]"; }
        }
        check(ok, "7a state machine follows Table I incl. hysteresis (12 steps)", bad);
        check(c->occ_transitions == 7, "7a transition count", fmt("(%.0f, expected %.0f)", c->occ_transitions, 7));
        long sum = c->occ_mode_cycles[0] + c->occ_mode_cycles[1] + c->occ_mode_cycles[2];
        check(sum == (long)seq.size(), "7a mode residency sums to controller cycles", fmt("(%.0f of %.0f)", sum, seq.size()));
    }

    // Workload A (as in Step 6): 24 core reads + 24 DMA reads, interleaved, 1 MB stride.
    std::vector<Req> A;
    for (int i = 0; i < 24; i++) {
        A.push_back(Req((long)i * 0x100000L + 0x10000000L, false, false));
        A.push_back(Req((long)i * 0x100000L + 0x60000000L, true,  false));
    }
    // ---- 7b: degenerate thresholds reproduce the static policies exactly ----
    {
        Result CF = run(make_cfg(base, dir, {"src_policy = CoreFirst"}), A);
        Result FR = run(make_cfg(base, dir, {"src_policy = FRFCFS"}), A);
        Result DF = run(make_cfg(base, dir, {"src_policy = DMAFirst"}), A);
        Result oR = run(make_cfg(base, dir, {"src_policy = OCCARB", "occarb_theta_lo = 1.0", "occarb_theta_hi = 1.0"}), A);
        Result oN = run(make_cfg(base, dir, {"src_policy = OCCARB", "occarb_theta_lo = 0.0", "occarb_theta_hi = 1.0"}), A);
        Result oU = run(make_cfg(base, dir, {"src_policy = OCCARB", "occarb_theta_lo = 0.0", "occarb_theta_hi = 0.0"}), A);
        check(oR.done_at == CF.done_at && oR.modes[1] == 0 && oR.modes[2] == 0, "7b OCCARB(lo=hi=1) == CoreFirst, always RELAXED");
        check(oN.done_at == FR.done_at && oN.modes[0] == 0 && oN.modes[2] == 0, "7b OCCARB(lo=0,hi=1) == FRFCFS, always NEUTRAL");
        check(oU.done_at == DF.done_at && oU.modes[0] == 0 && oU.modes[1] == 0, "7b OCCARB(lo=hi=0) == DMAFirst, always URGENT");
    }
    // ---- 7c: fallback signal = own DMA queue fill ----
    {
        std::vector<Req> D;   // 64 sequential DMA reads at once -> dmaq full -> URGENT, drains -> RELAXED
        for (int i = 0; i < 64; i++) D.push_back(Req((long)i * 64 + 0x60000000L, true, false));
        Result r = run(make_cfg(base, dir, {"src_policy = OCCARB", "occarb_source = dmaq", "occarb_occ_bits = 2",
                                           "occarb_theta_lo = 0.25", "occarb_theta_hi = 0.75"}), D);
        check(r.modes[2] > 0, "7c dmaq source: full DMA queue drives URGENT", fmt("(%.0f URGENT cycles, %.0f transitions)", r.modes[2], r.transitions));
        check(r.transitions >= 2 && r.modes[0] > 0, "7c dmaq source: returns to RELAXED once drained");
        Result n = run(make_cfg(base, dir, {"src_policy = OCCARB", "occarb_theta_lo = 0.25", "occarb_theta_hi = 0.75"}), D);
        check(n.modes[2] == 0 && n.transitions == 0, "7c nic source without a sideband: stays RELAXED (o = 0)");
    }
    // ---- 7d: relaxed_strict option ----
    {
        std::string cCF = make_cfg(base, dir, {"src_policy = CoreFirst"});
        std::string cST = make_cfg(base, dir, {"src_policy = CoreFirst", "relaxed_strict = on"});
        // A has no ready DMA aimed at a bank with waiting core requests -> strict must change nothing
        Result aCF = run(cCF, A), aST = run(cST, A);
        check(aST.done_at == aCF.done_at && aST.blocked == 0, "7d strict == CoreFirst when no bank is shared");
        // Find bank X (core writes + DMA reads, different rows) and bank Y != X (core reads).
        long X0 = 0x20000000L;
        std::vector<int> dx = decode(cCF, X0);
        std::vector<long> xs;                       // addresses in bank X, distinct rows
        for (long k = 1; xs.size() < 24 && k < 100000; k++) {
            long a = X0 + k * 0x2000L; std::vector<int> d = decode(cCF, a);
            if (same_bank(d, dx) && d.back() != dx.back()) xs.push_back(a);
        }
        long Y = -1;
        for (long k = 1; k < 100000 && Y < 0; k++) { long a = X0 + k * 0x40L; if (!same_bank(decode(cCF, a), dx)) Y = a; }
        check(xs.size() == 24 && Y >= 0, "7d found 24 same-bank rows (bank X) and a different bank Y");
        std::vector<Req> S;
        for (int i = 0; i < 8; i++)  S.push_back(Req(xs[i], false, true));            // core writes wait in writeq (bank X)
        for (int i = 0; i < 16; i++) S.push_back(Req(Y + i * 0x10000000L, false, false)); // core reads keep read mode
        for (int i = 8; i < 24; i++) S.push_back(Req(xs[i], true, false));            // DMA reads to bank X
        Result sCF = run(cCF, S), sST = run(cST, S);
        bool allDone = true; for (long t : sST.done_at) allDone = allDone && t >= 0;
        check(allDone, "7d strict: every request still completes (no starvation)");
        check(sST.blocked > 0 && sCF.blocked == 0, "7d strict: DMA held back from a bank with queued core writes",
              fmt("(%.0f blocked decisions vs %.0f)", sST.blocked, sCF.blocked));
        double wCF = 0, wST = 0; for (int i = 0; i < 8; i++) { wCF += sCF.done_at[i]; wST += sST.done_at[i]; }
        check(wST <= wCF, "7d strict: the bank-X core writes finish no later than without strict", fmt("(%.0f vs %.0f ticks mean)", wST / 8, wCF / 8));
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL OCCARB TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
