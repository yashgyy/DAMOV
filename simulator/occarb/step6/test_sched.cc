// OCCARB Step 6 unit tests: source arbitration policies in the Ramulator controller.
// Drives Ramulator directly through RamulatorWrapper (the interface ZSim uses), no ZSim/Pin.
// Usage: test_sched <base DDR4 cfg> <scratch dir>     (built and run by occarb/step6/run_check.sh)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <functional>
#include "RamulatorWrapper.h"
#include "Request.h"
using namespace ramulator;

static int fails = 0;
static void check(bool ok, const std::string& name, const std::string& detail = "") {
    printf("%s %s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    if (!ok) fails++;
}

struct Req { long addr; bool dma; bool write; long at;  // at = send tick
             Req(long a, bool d, bool w, long t = 0) : addr(a), dma(d), write(w), at(t) {} };
struct Result { std::vector<long> done_at; std::vector<int> order; long ticks; };

// Write <base cfg> + "src_policy = <policy>" (empty policy = key absent) to a new cfg file.
static std::string make_cfg(const std::string& base, const std::string& dir, const std::string& policy) {
    std::string path = dir + "/sched_" + (policy.empty() ? "default" : policy) + ".cfg";
    std::ifstream in(base); std::ofstream out(path);
    std::string line;
    while (std::getline(in, line)) if (line.find("src_policy") == std::string::npos) out << line << "\n";
    if (!policy.empty()) out << " src_policy = " << policy << "\n";
    return path;
}

// Send all requests at t=0 (in the given order), tick until all complete.
static Result run(const std::string& cfg, const std::vector<Req>& reqs) {
    RamulatorWrapper w(cfg.c_str(), 1, 64, false, false, "occarb_step6_test", false);
    Result r; r.done_at.assign(reqs.size(), -1); r.ticks = 0;
    long now = 0; size_t done = 0;
    for (size_t i = 0; i < reqs.size(); i++) {      // requests must be listed in non-decreasing `at`
        while (now < reqs[i].at) { w.tick(); now++; }
        auto cb = [&, i](Request&) { r.done_at[i] = now; r.order.push_back((int)i); done++; };
        Request q(reqs[i].addr, reqs[i].write ? Request::Type::WRITE : Request::Type::READ, cb, 0);
        q.is_dma = reqs[i].dma;
        while (!w.send(q)) { w.tick(); now++; }
    }
    while (done < reqs.size() && now < 10000000) { w.tick(); now++; }
    r.ticks = now;
    return r;
}

static double mean_lat(const Result& r, const std::vector<Req>& reqs, bool dma) {
    double s = 0; int n = 0;
    for (size_t i = 0; i < reqs.size(); i++) if (reqs[i].dma == dma) { s += r.done_at[i]; n++; }
    return n ? s / n : 0;
}
static int first_k_of(const Result& r, const std::vector<Req>& reqs, int k, bool dma) {  // how many of the first k completions are dma/core
    int c = 0;
    for (int j = 0; j < k && j < (int)r.order.size(); j++) if (reqs[r.order[j]].dma == dma) c++;
    return c;
}
static std::string fmt(const char* f, double a, double b) { char buf[160]; snprintf(buf, sizeof buf, f, a, b); return buf; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <base cfg> <scratch dir>\n", argv[0]); return 2; }
    std::string base = argv[1], dir = argv[2];
    std::string cDef = make_cfg(base, dir, ""), cFR = make_cfg(base, dir, "FRFCFS"),
                cCF = make_cfg(base, dir, "CoreFirst"), cDF = make_cfg(base, dir, "DMAFirst");

    // Workload A: 24 core reads + 24 DMA reads, interleaved arrival, 1 MB stride (row misses/conflicts).
    std::vector<Req> A;
    for (int i = 0; i < 24; i++) {
        A.push_back({(long)i * 0x100000L + 0x10000000L, false, false});
        A.push_back({(long)i * 0x100000L + 0x60000000L, true,  false});
    }
    Result aDef = run(cDef, A), aFR = run(cFR, A), aCF = run(cCF, A), aDF = run(cDF, A);
    bool allDone = true;
    for (auto* r : {&aDef, &aFR, &aCF, &aDF}) for (long t : r->done_at) allDone = allDone && t >= 0;
    check(allDone, "6a every request completes under every policy");
    check(aDef.done_at == aFR.done_at, "6a no src_policy key == FRFCFS (identical completion times)");
    double cc = mean_lat(aCF, A, false), cd = mean_lat(aCF, A, true);
    check(cc < cd, "6b CoreFirst: core reads finish earlier than DMA reads", fmt("(core %.0f vs dma %.0f ticks)", cc, cd));
    check(first_k_of(aCF, A, 12, false) >= 10, "6b CoreFirst: >=10 of the first 12 completions are core",
          fmt("(%.0f of %.0f)", first_k_of(aCF, A, 12, false), 12));
    double dc = mean_lat(aDF, A, false), dd = mean_lat(aDF, A, true);
    check(dd < dc, "6c DMAFirst: DMA reads finish earlier than core reads", fmt("(dma %.0f vs core %.0f ticks)", dd, dc));
    check(first_k_of(aDF, A, 12, true) >= 10, "6c DMAFirst: >=10 of the first 12 completions are DMA",
          fmt("(%.0f of %.0f)", first_k_of(aDF, A, 12, true), 12));
    double fc = mean_lat(aFR, A, false), fd = mean_lat(aFR, A, true);
    check(cc <= fc && fc <= dc, "6d core latency ordering CoreFirst <= FRFCFS <= DMAFirst",
          fmt("(FR-FCFS core %.0f, dma %.0f)", fc, fd));

    // Workload A2: DMA reads are OLDER (sent at t=0), core reads arrive at t=20.
    // FR-FCFS is age-based across the two queues -> serves DMA first; CoreFirst still serves core first.
    std::vector<Req> A2;
    for (int i = 0; i < 24; i++) A2.push_back({(long)i * 0x100000L + 0x60000000L, true, false, 0});
    for (int i = 0; i < 24; i++) A2.push_back({(long)i * 0x100000L + 0x10000000L, false, false, 20});
    Result a2FR = run(cFR, A2), a2CF = run(cCF, A2);
    double a2fc = mean_lat(a2FR, A2, false) - 20, a2fd = mean_lat(a2FR, A2, true);
    double a2cc = mean_lat(a2CF, A2, false) - 20, a2cd = mean_lat(a2CF, A2, true);
    check(a2fd < a2fc, "6d FRFCFS is age-based across queues: older DMA served before younger core",
          fmt("(dma %.0f vs core %.0f ticks latency)", a2fd, a2fc));
    check(a2cc < a2fc, "6d CoreFirst overtakes older DMA: core latency lower than under FRFCFS",
          fmt("(CoreFirst %.0f vs FRFCFS %.0f)", a2cc, a2fc));
    check(a2cd > a2fd, "6d ... at the cost of DMA latency", fmt("(CoreFirst %.0f vs FRFCFS %.0f)", a2cd, a2fd));

    // Workload B: DMA *writes* vs core reads. The DMA queue is separate, so DMA writes must not
    // push the controller into write-drain mode ahead of core reads under CoreFirst.
    std::vector<Req> B;
    for (int i = 0; i < 24; i++) {
        B.push_back({(long)i * 0x100000L + 0x10000000L, false, false});
        B.push_back({(long)i * 0x100000L + 0x60000000L, true,  true});
    }
    Result bCF = run(cCF, B), bDF = run(cDF, B);
    double bcc = mean_lat(bCF, B, false), bcd = mean_lat(bCF, B, true);
    check(bcc < bcd, "6e CoreFirst: core reads beat DMA writes (no DMA-triggered write drain)", fmt("(core %.0f vs dma %.0f)", bcc, bcd));
    double bdc = mean_lat(bDF, B, false), bdd = mean_lat(bDF, B, true);
    check(bdd < bdc, "6e DMAFirst: DMA writes beat core reads", fmt("(dma %.0f vs core %.0f)", bdd, bdc));

    // Workload C: core-only traffic (reads + writes). Policies must not change anything without DMA.
    std::vector<Req> C;
    for (int i = 0; i < 40; i++) C.push_back({(long)(i * 7919 % 64) * 0x40000L + 0x20000000L, false, i % 3 == 0});
    Result cFRr = run(cFR, C), cCFr = run(cCF, C), cDFr = run(cDF, C), cDefr = run(cDef, C);
    check(cDefr.done_at == cFRr.done_at && cFRr.done_at == cCFr.done_at && cFRr.done_at == cDFr.done_at,
          "6f core-only traffic: identical completion times under all policies");

    // Workload D: DMA-only traffic, also policy-independent.
    std::vector<Req> D;
    for (int i = 0; i < 40; i++) D.push_back({(long)i * 64 + 0x60000000L, true, i % 2 == 0});
    Result dFR = run(cFR, D), dCF = run(cCF, D), dDF = run(cDF, D);
    check(dFR.done_at == dCF.done_at && dFR.done_at == dDF.done_at, "6f DMA-only traffic: identical completion times under all policies");

    printf(fails ? "\n%d FAILED\n" : "\nALL SCHEDULER TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
