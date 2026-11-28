#!/usr/bin/env python
# OCCARB Step 7 patch: applies all source edits for occupancy-driven arbitration.
# Run from simulator/:  python3 occarb/step7/apply_step7.py      (python 2 also works)
# Every anchor must occur exactly once; if any check fails, NO file is modified.
from __future__ import print_function
import io, sys
EDITS = [
    ('ramulator/Controller.h',
     '    //   DMAFirst  : DMA request if one can issue (row hits first); else core       [URGENT]\n    enum class SrcPolicy { FRFCFS, CoreFirst, DMAFirst } src_policy = SrcPolicy::FRFCFS;\n    long dma_cmds = 0, core_cmds = 0;  // requests started, per source (for checks)\n',
     '    //   DMAFirst  : DMA request if one can issue (row hits first); else core       [URGENT]\n    //   OCCARB    : switches between the three above from the I/O buffer occupancy (Step 7)\n    enum class SrcPolicy { FRFCFS, CoreFirst, DMAFirst, OCCARB } src_policy = SrcPolicy::FRFCFS;\n    long dma_cmds = 0, core_cmds = 0;  // requests started, per source (for checks)\n'),
    ('ramulator/Controller.h',
     '    long dma_cmds = 0, core_cmds = 0;  // requests started, per source (for checks)\n\n',
     '    long dma_cmds = 0, core_cmds = 0;  // requests started, per source (for checks)\n\n    // OCCARB (paper Sec. II-B, Table I): RELAXED (o < lo) -> CoreFirst, NEUTRAL (lo <= o < hi) -> FRFCFS,\n    // URGENT (o >= hi, held until o < lo) -> DMAFirst.\n    enum class OccMode { RELAXED = 0, NEUTRAL = 1, URGENT = 2 } occ_mode = OccMode::RELAXED;\n    double occ_theta_lo = 0.25, occ_theta_hi = 0.75;\n    bool occ_from_dmaq = false;        // fallback signal: own DMA queue fill instead of the NIC sideband\n    int occ_bits = 2;                  // quantisation of the DMA-queue signal\n    int io_level = 0, io_levels = 4;   // last level received over the NIC sideband\n    bool relaxed_strict = false;       // option: in RELAXED/CoreFirst, no DMA to a bank with any queued core request\n    long occ_transitions = 0, occ_mode_cycles[3] = {0, 0, 0}, strict_blocked = 0;\n    ScalarStat occ_relaxed_cycles, occ_neutral_cycles, occ_urgent_cycles, occ_transitions_stat;\n\n'),
    ('ramulator/Controller.h',
     '        else if (configs["src_policy"] == "DMAFirst")  src_policy = SrcPolicy::DMAFirst;\n        else { printf("[RAMULATOR] unknown src_policy \'%s\'\\n", configs["src_policy"].c_str()); assert(false); exit(1); }\n',
     '        else if (configs["src_policy"] == "DMAFirst")  src_policy = SrcPolicy::DMAFirst;\n        else if (configs["src_policy"] == "OCCARB")    src_policy = SrcPolicy::OCCARB;\n        else { printf("[RAMULATOR] unknown src_policy \'%s\'\\n", configs["src_policy"].c_str()); assert(false); exit(1); }\n'),
    ('ramulator/Controller.h',
     '                   readq.max, writeq.max, dmaq.max, configs["src_policy"] == "" ? "FRFCFS" : configs["src_policy"].c_str());\n        if (with_drampower) {\n',
     '                   readq.max, writeq.max, dmaq.max, configs["src_policy"] == "" ? "FRFCFS" : configs["src_policy"].c_str());\n        // OCCARB parameters\n        if (configs["occarb_theta_lo"] != "") occ_theta_lo = stod(configs["occarb_theta_lo"]);\n        if (configs["occarb_theta_hi"] != "") occ_theta_hi = stod(configs["occarb_theta_hi"]);\n        if (configs["occarb_occ_bits"] != "") occ_bits = stoi(configs["occarb_occ_bits"]);\n        occ_from_dmaq = (configs["occarb_source"] == "dmaq");\n        relaxed_strict = (configs["relaxed_strict"] == "on" || configs["relaxed_strict"] == "true");\n        if (occ_theta_lo > occ_theta_hi) { printf("[RAMULATOR] occarb_theta_lo > occarb_theta_hi\\n"); assert(false); exit(1); }\n        if (src_policy == SrcPolicy::OCCARB) {\n            string ch = to_string(channel->id);\n            occ_relaxed_cycles.name("occarb_relaxed_cycles_" + ch).desc("Controller cycles in RELAXED mode").precision(0);\n            occ_neutral_cycles.name("occarb_neutral_cycles_" + ch).desc("Controller cycles in NEUTRAL mode").precision(0);\n            occ_urgent_cycles.name("occarb_urgent_cycles_" + ch).desc("Controller cycles in URGENT mode").precision(0);\n            occ_transitions_stat.name("occarb_transitions_" + ch).desc("OCCARB mode changes").precision(0);\n            if (channel->id == 0)\n                printf("[RAMULATOR] OCCARB: theta_lo=%.4f theta_hi=%.4f source=%s occ_bits(dmaq)=%d\\n",\n                       occ_theta_lo, occ_theta_hi, occ_from_dmaq ? "dmaq" : "nic", occ_bits);\n        }\n        if (relaxed_strict && channel->id == 0) printf("[RAMULATOR] relaxed_strict=on\\n");\n        if (with_drampower) {\n'),
    ('ramulator/Controller.h',
     '        (*req_queue_length_sum) += readq.size() + writeq.size() + dmaq.size() + pending.size();\n        (*read_req_queue_length_sum) += readq.size() + pending.size();\n',
     '        (*req_queue_length_sum) += readq.size() + writeq.size() + dmaq.size() + pending.size();\n        if (src_policy == SrcPolicy::OCCARB) occ_update();  // OCCARB: mode for this cycle\n        (*read_req_queue_length_sum) += readq.size() + pending.size();\n'),
    ('ramulator/Controller.h',
     '\n    // OCCARB: true if the DMA candidate should be served instead of the core candidate.\n',
     "\n    // OCCARB: occupancy signal o in [0,1), already quantised (the paper's 2-bit level / levels).\n    void set_io_level(int level, int levels) { io_level = level; io_levels = levels; }\n    double occupancy()\n    {\n        if (!occ_from_dmaq) return (double)io_level / io_levels;\n        int levels = 1 << occ_bits;\n        int l = (int)((double)dmaq.size() * levels / dmaq.max);\n        return (double)min(max(l, 0), levels - 1) / levels;\n    }\n    // OCCARB: two threshold comparisons + hysteresis (Fig. 1b). Called once per controller cycle.\n    void occ_update()\n    {\n        double o = occupancy();\n        OccMode m = occ_mode;\n        if (m == OccMode::URGENT) { if (o < occ_theta_lo) m = OccMode::RELAXED; }\n        else if (o >= occ_theta_hi) m = OccMode::URGENT;\n        else if (o >= occ_theta_lo) m = OccMode::NEUTRAL;\n        else m = OccMode::RELAXED;\n        if (m != occ_mode) { occ_transitions++; ++occ_transitions_stat; occ_mode = m; }\n        occ_mode_cycles[int(m)]++;\n        if (m == OccMode::RELAXED) ++occ_relaxed_cycles;\n        else if (m == OccMode::NEUTRAL) ++occ_neutral_cycles;\n        else ++occ_urgent_cycles;\n    }\n    // OCCARB option relaxed_strict: is any core request (read or write queue) waiting for this bank?\n    bool bank_has_core_req(list<Request>::iterator dma)\n    {\n        int lb = int(T::Level::Bank);\n        auto same_bank = [&](const Request& r) {\n            for (int l = 0; l <= lb; l++) if (r.addr_vec[l] != dma->addr_vec[l]) return false;\n            return true; };\n        for (auto& r : readq.q)  if (same_bank(r)) return true;\n        for (auto& r : writeq.q) if (same_bank(r)) return true;\n        return false;\n    }\n\n    // OCCARB: true if the DMA candidate should be served instead of the core candidate.\n"),
    ('ramulator/Controller.h',
     '        bool cr = is_ready(core), dr = is_ready(dma);\n        switch (src_policy) {\n            case SrcPolicy::CoreFirst: return !cr && dr;\n            case SrcPolicy::DMAFirst:  return dr || !cr;\n',
     '        bool cr = is_ready(core), dr = is_ready(dma);\n        SrcPolicy p = src_policy;\n        if (p == SrcPolicy::OCCARB)\n            p = occ_mode == OccMode::RELAXED ? SrcPolicy::CoreFirst :\n                occ_mode == OccMode::NEUTRAL ? SrcPolicy::FRFCFS : SrcPolicy::DMAFirst;\n        switch (p) {\n            case SrcPolicy::CoreFirst:\n                if (!(!cr && dr)) return false;\n                if (relaxed_strict && bank_has_core_req(dma)) { strict_blocked++; return false; }\n                return true;\n            case SrcPolicy::DMAFirst:  return dr || !cr;\n'),
    ('ramulator/Memory.h',
     '    virtual void set_application_name(string) = 0;\n};\n',
     '    virtual void set_application_name(string) = 0;\n    virtual void set_io_level(int level, int levels) {}  // OCCARB: NIC occupancy sideband (no-op by default)\n};\n'),
    ('ramulator/Memory.h',
     '\n    int pending_requests()\n',
     '\n    // OCCARB: forward the NIC occupancy level to every controller\n    void set_io_level(int level, int levels) { for (auto ctrl : ctrls) ctrl->set_io_level(level, levels); }\n\n    int pending_requests()\n'),
    ('ramulator/RamulatorWrapper.h',
     '    double get_tCK();\n};\n',
     '    double get_tCK();\n    void set_io_level(int level, int levels);  // OCCARB: NIC occupancy sideband\n};\n'),
    ('ramulator/RamulatorWrapper.cc',
     '\ndouble RamulatorWrapper::get_tCK() {\n',
     '\nvoid RamulatorWrapper::set_io_level(int level, int levels) {  // OCCARB\n    mem->set_io_level(level, levels);\n}\n\ndouble RamulatorWrapper::get_tCK() {\n'),
    ('src/ramulator_mem_ctrl.cpp',
     '  nicLevelCycles[nic->level()]++;\n\n',
     '  nicLevelCycles[nic->level()]++;\n  wrapper->set_io_level(nic->level(), 1 << nic->p.occ_bits);  // OCCARB: occupancy sideband to the controller\n\n'),
]

def main():
    texts, ok = {}, True
    for f, old, new in EDITS:
        if f not in texts:
            texts[f] = io.open(f, encoding="utf-8").read()
        t = texts[f]
        if t.count(new) == 1:
            print("SKIP (already applied) in %s: %s" % (f, new.strip().splitlines()[0][:70]))
        elif t.count(old) == 1:
            texts[f] = t.replace(old, new)
        else:
            ok = False
            print("ERROR in %s: anchor found %d times: %s" % (f, t.count(old), old.strip().splitlines()[0][:70]))
    if not ok:
        print("\nNothing was changed. Paste these ERROR lines to Claude.")
        sys.exit(1)
    for f, t in texts.items():
        io.open(f, "w", encoding="utf-8").write(t)
        print("patched", f)
    print("STEP 7 PATCH APPLIED (%d edits)" % len(EDITS))

if __name__ == "__main__":
    main()
