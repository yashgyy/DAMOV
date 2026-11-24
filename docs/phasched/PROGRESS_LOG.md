# PhaSched — Progress Log

Short, dated record of what was decided and done. Newest entries at the bottom of each section.
Implementation steps are in `IMPLEMENTATION_GUIDE.md`.

---

## 1. Project summary

- **Paper:** *Round-Phase-Aware Memory Scheduling for Federated Learning Clients* (PhaSched, late-breaking results).
- **Idea:** the FL runtime writes the round phase (IDLE / INGRESS / TRAIN / EGRESS) into a 2-bit register on the
  memory controller; the phase selects a preset of three existing FR-FCFS knobs: write-drain watermarks Whi/Wlo and
  row-hit cap C, plus a starvation guard Tmax.
- **Simulator:** DAMOV-SIM (ZSim + Ramulator), full-system mode (not trace-driven). Pin/ZSim already run.
- **Deadline:** 4 days.

## 2. Decisions

| Date | Decision | Reason |
|---|---|---|
| 2026-11-23 | Use full ZSim + Ramulator (not trace-driven) | DAMOV/Pin already work on the user's setup |
| 2026-11-23 | Only the FL **client** is simulated; server runs natively or as a stub (`-DFL_STUB_SERVER`) | ZSim does not time network I/O; stub gives determinism |
| 2026-11-23 | NIC modelled by a **DMA injector** in `src/ramulator_mem_ctrl.cpp`, cache-bypassing, paced at the link rate | DAMOV has no NIC |
| 2026-11-23 | Phase hint = new ZSim magic ops 1040–1043 (phase, DMA write, DMA read, DMA poll) | Reuses existing `xchg rcx,rcx` mechanism |
| 2026-11-24 | Workloads: logistic regression, naive Bayes, DNN (classifier or regression), 2-layer LSTM, GRU — all C++ | User's existing FL client/server code; diverse memory behaviour |
| 2026-11-24 | Model sizes set so transfer matters: LogReg 1M×10 (~40 MB), NB 1M×10 (~40 MB), DNN 784-2048-2048-10 (~23 MB), LSTM 2×1024/in 512 (~30 MB), GRU 2×1024/in 512 (~23 MB) | Tiny models → INGRESS/EGRESS ≈ 0 → no measurable effect |
| 2026-11-24 | Drop linear regression; use either DNN classifier or DNN regression (not both) | Near-duplicate memory behaviour |
| 2026-11-24 | Baselines: FCFS, FR-FCFS (`FRFCFS`), FR-FCFS-Cap (`FRFCFS_Cap`, C=16), BLISS (new), PhaSched (new) | Paper Section III |
| 2026-11-24 | PhaSched built on FR-FCFS-Cap: IDLE preset == FR-FCFS-Cap (0.8/0.2, C=16) | Makes "IDLE = baseline" exact and testable |
| 2026-11-24 | Energy from per-phase DRAM command counts × Micron DDR4 IDD-based energies | DAMOV Ramulator has `with_drampower = false` hard-coded |
| 2026-11-24 | Add model-size sweep (LogReg 1/10/40 MB) | Shows when PhaSched pays off; pre-empts reviewer question |

## 3. Simulated configuration (Paper Table II)

| Row | Value | Source |
|---|---|---|
| Cores | 4 × 4-wide OOO x86-64, 2.4 GHz | DAMOV `template_host_ooo.cfg` |
| Caches | 32 kB L1-D (8-way, 4 cyc), 32 kB L1-I, 256 kB L2 (8-way, 7 cyc), 8 MB shared L3 (16-way, 16 banks, 27 cyc) | DAMOV template defaults |
| Memory | DDR4-2400 (DDR4_4Gb_x8), 1 channel, 1 rank, 32-entry read/write queues | `DDR4-config.cfg`, Ramulator default `Queue::max = 32` |
| Baseline scheduler | FR-FCFS, write drain 80 % / 20 %, cap 16 for FR-FCFS-Cap | Ramulator defaults |
| Network | 10 GbE (1.25 GB/s → 122.88 CPU cycles per 64 B line), DMA injector | chosen |
| Workloads | FedAvg clients (C++): LogReg, naive Bayes, DNN, 2-layer LSTM, GRU | user |
| Round | E = 1 local epoch (truncated to K minibatches if needed), B = 32 | chosen; K fixed in Step 7 |

## 4. Step status

| Step | Description | Status | Date | Notes |
|---|---|---|---|---|
| 0 | Baseline + ubench + golden output | ◐ | 2026-11-24 | Instructions given; waiting for user's results |
| 1 | Config knobs (scheduler, queue, Whi/Wlo, cap) | ☐ | | |
| 2 | Phase interface (magic op → phase register) | ☐ | | |
| 3 | Per-phase metrics CSV | ☐ | | |
| 4 | PhaSched presets + starvation guard | ☐ | | |
| 5 | BLISS baseline | ☐ | | |
| 6 | DMA injector (NIC) | ☐ | | |
| 7 | FL workload integration + round sizing (K) | ☐ | | |
| 8 | Experiment matrix runs | ☐ | | |
| 9 | Analysis, energy, figures, paper | ☐ | | |

Legend: ☐ not started · ◐ in progress · ☑ done and verified

## 5. Step log

Template — copy for every step:

```
### Step N — <title>  (YYYY-MM-DD)
- Changed: <files, one line each>
- Build: OK / issues
- Verification: <which checks passed, key numbers>
- Commit: <hash>
- Problems / deviations: <none or short note>
```

### Step 0 — Baseline + test harness  (2026-11-24, in progress)
- To create: `phasched/include/phase_hint.h` (no-op hints, CPU stands in for NIC), `phasched/ubench/ubench.c`
  (one synthetic FL round: seq write 16 MB / random reads 64 MB / seq read 16 MB),
  `phasched/configs/zsim/base_ooo4_ddr4.cfg` (template_host_ooo, 4 cores, DDR4-config.cfg, maxTotalInstrs 20e9)
- Checks: runs native + in ZSim; stats show DDR4; two runs identical; golden saved to `phasched/results/golden/step0.*`
- Issues hit & fixed:
  1. VS Code `EACCES` on new files — files created as root inside Docker container; fix with `chown` or attach VS Code to container.
  2. `ubench: No such file or directory` — host path used; container path is `/DAMOV/...`.
  3. Ramulator assertion `!stat_output.good()` — `stats` must be relative to `/DAMOV/simulator` (`../phasched/results/ubench_base`).
- Native run OK; ZSim run completes (DDR4 @1200 MHz, ROI begin/end, `Finished, code 0`).
- Outputs (in `/DAMOV/phasched/results/`): `ubench_base.ramulator.stats`, `.zsim.out`, `.out.cfg`,
  `.dramRequestsPerPhase` (16 MB — DAMOV per-phase DRAM request dump; watch size for long runs).
- Host user uid:gid = 1037:1041 → `chown -R 1037:1041 /DAMOV/phasched` inside container fixes VS Code permissions.
- Run time ≈ 5 min (16:47 → 16:52) for ubench 16 64 1.
- Determinism: two runs → `ramulator.stats` IDENTICAL ✅
- Result: checks 1, 2, 4 passed; check 3 values and golden copy _pending_

## 6. Results (fill in Step 9)

| Metric | Value |
|---|---|
| [N] workloads | 5 |
| [X%] avg round-time reduction vs FR-FCFS | |
| [Xmax%] best case | |
| [A%] INGRESS reduction | |
| [B%] EGRESS reduction | |
| [T%] turnaround reduction | |
| [Y%] TRAIN read-latency reduction | |
| [Z%] BLISS gain | |
| [E%] DRAM energy change | |

## 7. Open items / risks

- LSTM/GRU simulation speed → may need K-minibatch truncation (disclose in paper).
- Paper text to update: abstract [N], intro "tens of megabytes for common vision networks [4]", Table II workloads row,
  Fig. 2 x-axis (5 workloads + geomean).
- Queue depth: paper placeholder says 64, DAMOV default is 32 — decided 32 unless changed here.
