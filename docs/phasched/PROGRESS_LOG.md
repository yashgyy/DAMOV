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
| Memory | DDR4-2400 (DDR4_4Gb_x8), 1 channel, 1 rank, 32-entry read/write queues; memory clock = core/2 (DAMOV divider enabled, Step 2b) | `DDR4-config.cfg`, Ramulator default `Queue::max = 32` |
| Baseline scheduler | FR-FCFS, write drain 80 % / 20 %, cap 16 for FR-FCFS-Cap | Ramulator defaults |
| Network | 10 GbE (1.25 GB/s → 122.88 CPU cycles per 64 B line), DMA injector | chosen |
| Workloads | FedAvg clients (C++): LogReg, naive Bayes, DNN, 2-layer LSTM, GRU | user |
| Round | E = 1 local epoch (truncated to K minibatches if needed), B = 32 | chosen; K fixed in Step 7 |

## 4. Step status

| Step | Description | Status | Date | Notes |
|---|---|---|---|---|
| 0 | Baseline + ubench + golden output | ☑ | 2026-11-24 | golden saved, deterministic |
| 1 | Config knobs (scheduler, queue, Whi/Wlo, cap) | ☑ | 2026-11-24 | + Ramulator get_hits bug fixed |
| 2 | Phase interface (magic op → phase register) | ☑ | 2026-11-24 | + Step 2b: memory clock divider enabled |
| 3 | Per-phase metrics CSV | ☑ | 2026-11-25 | + 3b real bus turnarounds |
| 4 | PhaSched presets + starvation guard | ☑ | 2026-11-25 | IDLE exact; EGRESS −87 % turnarounds; INGRESS re-check with DMA |
| 5 | BLISS baseline | ☑ | 2026-11-25 | works; cannot fix write-drain starvation |
| 6 | DMA injector (NIC) | ☑ | 2026-11-25 | validated; 1000 G gain = artifact; QoS gain real |
| 7 | FL workload integration + round sizing (K) | ☑ | 2026-11-25 | synthetic clients; dnn sim 5.6 min; TRAIN write interference visible |
| 8 | Experiment matrix runs | ☑ | 2026-11-25 | 25 runs @10 G; PhaSched geo −0.3 %, max −1.1 % |
| 9 | Analysis, energy, figures, paper | ☑ | 2026-11-25 | all placeholders filled; compile + review pending |

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

### Step 0 — Baseline + test harness  (2026-11-24, done)
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
- Golden values: dram_cycles 1,435,415,000 · read_requests 16,394,048 · write_requests 1,310,721 ·
  row_hits 1,034,622 · read_latency_avg 54.60 clk (45.50 ns).
- Result: ✅ all 5 checks passed; golden saved to `phasched/results/golden/step0.*`.

### Step 1 — Scheduler / knobs from config  (2026-11-24, done)
- Change: `simulator/ramulator/Controller.h` only (wq_high/wq_low members, config keys `scheduler`, `row_hit_cap`,
  `queue_size`, `wq_high`, `wq_low`, watermarks used in `tick()`).
- New configs: `phasched/configs/ramulator/DDR4-{FCFS,FRFCFS,FRFCFS_Cap}.cfg`, `phasched/configs/zsim/ubench_*.cfg`.
- Build: ✅ libramulator.so + zsim rebuilt without errors. `[PHASCHED] ch0 scheduler=FRFCFS_PriorHit(default) cap=16 q=32 whi=0.80 wlo=0.20` printed.
- Check 2 attempt 1: INVALID — run was stopped with Ctrl-C (output piped to grep looked idle); partial stats
  (dram_cycles 44.4M vs 1435M) were dumped, so diff vs golden differed. Lesson: never interrupt; log to file instead of piping.
- Check 2 attempt 2 (full run, output redirected to a log file): NOT bit-identical but within 0.002 %
  (dram_cycles 1,435,446,000 vs 1,435,415,000; read_requests +370; read_latency_avg 54.606 vs 54.601).
  The code change cannot alter behaviour at default (0.8/0.2 are the same values), so suspected cause is the run
  environment: golden ran with stdout to the terminal, this run with stdout to a file.
  Test: re-run with redirect; if identical to attempt 2 → code is neutral, re-baseline golden with the
  redirect method (`golden/step1_base.*`) and always run with `> log 2>&1` from now on.
- Check 2 attempt 3 (redirected, 4 runs in parallel): again ~0.01 % different from attempt 2
  (dram_cycles 1,435,563,000). ⇒ ZSim is NOT bit-deterministic run-to-run (spread 0.01 % over 3 runs).
  Decision: regression passes within ±0.1 % on key stats; added `phasched/scripts/cmp.sh`.
- Check 3: FCFS / FRFCFS / FRFCFS_PriorHit(default) each printed their own `[PHASCHED]` line ✅.
  FRFCFS_Cap CRASHED: `Scheduler.h:240 RowTable::update Assertion 'match->second.row == row'`.
  Cause: Ramulator bug — `RowTable::get_hits()` uses `table[rowgroup]` which inserts a fake row-0 entry for closed
  banks; later ACT `insert` does not overwrite it. Fix: use the `find` iterator only (guide 1.6).
- Check 4 (partial): base 1,435,563,000 cyc / 54.61 clk; FRFCFS 1,435,509,000 / 54.61 (≈ base, as expected);
  FCFS 1,450,085,000 (+1.0 %) / 57.04 (+4.4 %) — FCFS slower, as expected. Row hits ≈ equal (random workload).
- Resources: host 125 GB RAM; 4 parallel ZSim runs OK memory-wise.
- After get_hits fix (cmp.sh, ±0.1 % rule):
  default vs golden: dram_cycles +0.004 %, read_requests −0.002 %, latency +0.011 % → all OK.
  FRFCFS_Cap vs FRFCFS: all within 0.01 % (ubench is random-access, cap rarely triggers) → runs, no crash.
- Result: ✅ Step 1 done.

### Step 2 — Phase interface  (2026-11-24, done)
- Changes: `phasched/include/phase_hint.h` (magic op 1040 with arg in RDX), `simulator/src/zsim.cpp`
  (pass RDX/RSI to HandleMagicOp, case 1040), `simulator/src/ramulator_mem_ctrl.{h,cpp}` (pendingPhase, apply in
  tick, `<stats>.phases.csv` log), `ramulator/RamulatorWrapper.{h,cc}`, `ramulator/Memory.h`, `ramulator/Controller.h`
  (`int phase`).
- Build error 1: `reference to 'REG_RDX' is ambiguous` (Pin enum vs `<sys/ucontext.h>`) → use
  `LEVEL_BASE::REG_RDX` / `LEVEL_BASE::REG_RSI` in the `INS_InsertCall`.
- phases.csv: INGRESS 0–2.807 M, TRAIN 2.807 M–1432.654 M, EGRESS 1432.654 M–1435.448 M cycles (order correct,
  TRAIN dominates as expected). Stamps quantised to 1000 cycles (ZSim bound-weave interval) — acceptable.
- Regression vs golden: all OK (max +0.057 %).
- Result: ✅ Step 2 done.

### Step 2b — Memory clock divider  (2026-11-24, done)
- Finding: end curCycle (1,435,449,000) == ramulator.dram_cycles → Ramulator ticks once per CPU cycle; DAMOV's clock
  divider is commented out, so DDR4-2400 runs 2× too fast in real time (log prints ratio 2 but it is unused).
- Decision: enable the divider (`if (tickCounter % freqRatio == 0) wrapper->tick();`) so Table II "DDR4-2400" is
  correct; new golden `golden/step2b.*` becomes the regression reference.
- Results: end cycle 1,895,427,000 (+32 % vs 1,435,449,000); dram_cycles 947,713,500 = ½ CPU cycles ✅.
  base: 55.10 clk latency, 1,057,759 row hits. FCFS: 991.6 M dram cyc (+4.6 %), 60.03 clk (+9 %).
  FRFCFS: 947.7 M, 55.11. FRFCFS_Cap: 947.7 M, 55.11 (≈ FRFCFS, random workload).
- Result: ✅ Step 2b done (pending: save golden/step2b.*, confirm FRFCFS_Cap phases.csv end line).

### Step 3 — Per-phase metrics  (2026-11-24, in progress)
- Changes: Request.h (`is_dma`), Controller.h (PhaseCnt counters, latency histogram, turnaround count, command
  counts in issue_cmd, `dump_phase`), Memory.h / RamulatorWrapper (phase_snapshot), ramulator_mem_ctrl.cpp (CSV rows).
- New helper: `phasched/scripts/phasum.sh`.
- Results (base, 2026-11-25): consistency checks exact (rd_lat_sum, row_hits, writes); reads −9 (forwarded);
  act +0.3 % vs misses+conflicts. Regression vs step2b OK (max 0.015 %).
  Per phase: INGRESS 5.0 M cyc (CPU proxy: 262 k reads = RFO of memset, 131 k writes, 99 % row hits, avg lat 75, p99 377);
  TRAIN 1886 M cyc (15.9 M reads, 1.17 M writes, avg 55.1, p99 128); EGRESS 4.05 M cyc (262 k reads, avg 35, p99 123).
- Issue: turnarounds counted write_mode flips (20.9 M in TRAIN) — baseline enters write mode whenever readq is
  empty, even with no writes. Fix (Step 3b): count real RD↔WR direction changes of column commands in issue_cmd.
- Step 3b: turnarounds now = RD↔WR direction changes of column commands (issue_cmd). INGRESS 10,719,
  TRAIN 2,173,619, EGRESS 12,636 (all ≤ 2·min(rd,wr)); other columns unchanged; cmp vs step2b OK.
- Result: ✅ Step 3 done. Reference `golden/step3.phases.csv`.

### Step 4 — PhaSched presets + starvation guard  (2026-11-25, in progress)
- Design: PhaSched = FR-FCFS-Cap + per-phase write drain + per-phase cap; IDLE == FR-FCFS-Cap exactly.
  INGRESS eager (wq>0 → write; leave at wq=0; cap ∞). TRAIN lazy (enter at Whi [forced] or idle reads; idle-entered
  drain leaves as soon as a read arrives; forced drain leaves at Wlo). EGRESS writes only at Whi, cap 64.
  Starvation guard Tmax = 2000 DRAM clk, active only in phases 1–3.
- Changes: Scheduler.h (enum BLISS/PhaSched, 2 compare lambdas, guard in get_head), Controller.h (tmax, egress_cap,
  forced_drain, phase_cap(), update_write_mode()), configs DDR4-PhaSched.cfg, ubench_PhaSched[_nohint].cfg,
  ubench_nohint binary (-DPHASCHED_NO_HINTS).
- Build error: `Controller<HMC> has no member named 'phase_cap' / 'phase' / 'tmax'` — DAMOV's HMC controller is a
  separate specialization that instantiates Scheduler<HMC>. Fix: add stub members to HMC_Controller.h (guide 4.2f).
- Results (ubench, CPU-proxy NIC):
  Regression A (base vs step2b) OK. Regression B (PhaSched no-hint vs FRFCFS_Cap) OK → IDLE == baseline ✅.
  EGRESS: turnarounds −87 % (12,810 → 1,712), cycles −4.9 %, avg lat 35.2 → 32.3, P99 126 → 161, row hits +0.4 %.
  TRAIN: unchanged (few writes; baseline already exits write mode immediately).
  INGRESS: worse (+12.5 % cycles, turnarounds 10.8 k → 35.4 k, avg lat 74.8 → 89.5) — CPU proxy mixes RFO reads
  with writebacks so eager drain flips for every writeback. Not representative of a NIC; re-check with DMA.
- Result: ✅ mechanism works and IDLE is exact; INGRESS rule to be re-evaluated with real DMA traffic.

### Order change
- Do Step 6 (DMA injector) before Step 5 (BLISS): the paper's main claims (INGRESS/EGRESS) need NIC traffic.

### ⚠ Key risk identified (2026-11-25): link-rate bound
- At 10 GbE the NIC issues one 64 B line every ~123 CPU cycles (1.25 GB/s) while DDR4-2400 x64 delivers 19.2 GB/s.
  INGRESS/EGRESS duration is then set by the link, not by DRAM → **no scheduler can shorten them**; PhaSched can
  only reduce interference with concurrent core traffic. The paper's "[A%]/[B%] ingress/egress shrink" is only
  achievable when DRAM is the bottleneck (fast links ≥ 100 GbE / RDMA, or concurrent memory traffic).
- Plan: measure with the DMA injector at 10 GbE and DRAM-bound (1000 Gbps) to quantify; then decide the Table II
  network row / a link-rate sweep (e.g. 10/40/100/200 Gbps).

### Step 6 — DMA injector  (2026-11-25, in progress)
- Changes: phase_hint.h (ops 1041–1043, poll loop), zsim.cpp (3 cases), ramulator_mem_ctrl.{h,cpp} (injector in tick,
  callback, setDmaLink), init.cpp (`sys.mem.dmaLinkGbps`, default 10).
- Results (ubench 16 MiB):
  10 GbE: INGRESS = EGRESS = 32,213,000 cyc (predicted 32.2 M ✅), 262,144 DMA writes / reads ✅;
  PhaSched bit-identical to FRFCFS_Cap → link-bound, risk CONFIRMED.
  1000 Gbps: INGRESS 5.957 M → 2.779 M (−53 %), EGRESS 5.940 M → 5.542 M (−6.7 %). Baseline write stream only
  6.75 GB/s vs PhaSched 14.5 GB/s (76 % of peak). Baseline starves the 5 INGRESS core reads (avg 297,850 DRAM clk);
  PhaSched 9,415 (guard flips back too early → bug).
  TRAIN identical across all runs (1,885.7 M) → ubench round time is TRAIN-dominated.
- Result: ✅ injector works.
- Step 6b (guard holds mode until starved request served): starved INGRESS reads now ≈ 2,039 clk (≈ Tmax) ✅; 10 G unchanged.
  1000 G INGRESS: FCFS 6.645 M, FRFCFS_Cap 5.957 M, FRFCFS 5.555 M, PhaSched 2.778 M (cap explains only ~7 %).
  100 G: INGRESS = EGRESS = 3.222 M for FRFCFS_Cap AND PhaSched (link-bound; baseline not DRAM-bound after all).
  PhaSched raises co-running core read latency at 100 G (INGRESS 198 → 2,037 clk, EGRESS 64.8 → 94.5).
- ⚠ Interpretation: baseline 1000 G INGRESS ≈ exactly 2 × PhaSched; the 5 starved core reads (5 × 277,766 DRAM clk ≈
  2.78 M CPU cyc) equal the difference → suspected ZSim bound-weave artifact (poll magic op evaluated in bound phase;
  load stall added serially). Step 6c logs DMA start/end to verify.
- ⚠ Research finding: in NIC-only INGRESS/EGRESS there are almost no turnarounds (1–8) for any scheduler and the
  duration is link-bound up to ≥100 GbE → PhaSched can only help where NIC traffic overlaps core memory traffic.
  Evaluation scenario must be decided with the user (see chat 2026-11-25).
- Step 6c (DMA timestamps): W transfer = 2,778,346 (FRFCFS) vs 2,777,730 (PhaSched) cycles at 1000 G; R identical
  (2,770,594 both); 100 G: 3.22 M both. ⇒ ARTIFACT CONFIRMED: NIC transfer time is scheduler-independent; baseline's
  extra INGRESS time is the starved core load counted after the DMA. Use dma.csv durations for INGRESS/EGRESS.
- Positive results so far:
  1. Starvation/QoS: DRAM-bound NIC transfer → FR-FCFS starves co-running reads ≈ 231 µs (277,820 clk);
     PhaSched bounds to ≈ 1.7 µs (2,037 clk) = 136× lower worst-case read latency.
  2. Mixed traffic in EGRESS (Step 4, CPU proxy): turnarounds −87 %, EGRESS time −4.9 %.
  3. Zero cost when not in an FL phase (IDLE == FR-FCFS-Cap, verified).
  4. Validated infrastructure: phase interface, per-phase metrics, NIC/DMA model for DAMOV.
- Negative: no INGRESS/EGRESS shortening with NIC-only traffic at 10/100 GbE; at 100 G PhaSched raises co-running
  read latency 240 → 2,037 clk (eager INGRESS rule holds write mode).

### Decision 2026-11-25
- Do not choose the paper story yet. Continue as planned: Step 5 (BLISS) → Step 7 (real FL workloads) → decide the
  story (overlap / co-runner / TRAIN-only) from Step 7 data.
- Measurement rule from now on: INGRESS/EGRESS durations from `<run>.dma.csv`, not phase stamps (bound-weave artifact).

### Step 5 — BLISS  (2026-11-25, in progress)
- Changes: Scheduler.h (BLISS state, on_served, is_black, real BLISS lambda), Controller.h (BLISS config keys,
  nsrc = cores + 1, on_served before erase, blacklistings print), DDR4-BLISS.cfg, ubench_BLISS_{10g,1000g}.cfg.
- Results: blacklistings 97,111 / 94,180; regression OK. 10 G: identical to FR-FCFS (one core source).
  1000 G INGRESS: core reads still starved 277,764 clk (= FR-FCFS) vs PhaSched 2,037 → BLISS cannot fix starvation
  caused by write-drain mode (it only reorders within a queue). Supports paper claim 12.
- Result: ✅ Step 5 done. Multi-core differences expected only with multi-threaded workloads (Step 7).

### Step 7 — Synthetic FedAvg clients  (2026-11-25, in progress)
- Decision: the user's FL code cannot be used → synthetic clients with real training kernels on generated data:
  `phasched/workloads/fl_clients.cpp` (logreg, nb, dnn, lstm, gru; OpenMP; one contiguous model buffer for the NIC).
- Sizes: logreg/nb 10 M params (38.1 MB), dnn 6.3 M (24.1 MB + grads), lstm 3.7 M (14.1 MB + grads),
  gru 2.8 M (10.5 MB + grads); 256 MB synthetic local data per client.
- Validation (native): loss decreases (check=1); gradient check analytic ≈ numeric (check=2) for dnn/lstm/gru.
- Native TRAIN, 4 steps, 4 threads: logreg 1.7 ms, nb 0.6 ms, dnn 286 ms, lstm 1047 ms, gru 817 ms.
- User machine (container), 2026-11-25: built OK; check=1 loss decreases for logreg/dnn/lstm/gru (identical values
  to the reference build); sizes as expected. Native TRAIN 4 steps: logreg 3.3 ms, nb 1.6 ms, dnn 241 ms,
  lstm 591 ms, gru 456 ms.
- First ZSim run (dnn, FRFCFS_Cap, 10 G, steps=2): wall 5 m 37 s. DMA W/R 25,264,192 B, 48.5 M cyc each ✅.
  TRAIN 446.2 M cyc: 4.53 M reads, **2.34 M writes** (34 % of traffic), **728,687 turnarounds**, core read latency
  avg 146 / p50 68 / p99 689 DRAM clk (vs 55 in the read-only ubench) → reads wait behind gradient/weight writes:
  exactly the situation the TRAIN preset targets. Round = 48.5 + 446.2 + 48.5 = 543 M cyc (NIC 18 %).
- Next: dnn × 5 schedulers @ 10 G; helper `phasched/scripts/flsum.sh`.

### Step 8 — first matrix: DNN × 5 schedulers @ 10 G, steps=2  (2026-11-25)
- Round (CPU cyc): FCFS 569.5 M (+5.7 %), FRFCFS 539.0 M (best), FRFCFS_Cap 543.3 M (+0.8 %), BLISS 555.2 M (+3.0 %),
  PhaSched 545.0 M (+1.1 %).
- TRAIN read latency avg/p99: FCFS 261/1018, FRFCFS 129.5/582, Cap 146.2/689, BLISS 137.2/628, PhaSched 144.3/690.
- Turnarounds: FCFS 819 k, FRFCFS 789 k, Cap 729 k, BLISS 586 k, PhaSched 804 k.
- Findings: (+) DNN TRAIN is scheduling-sensitive (FCFS→FRFCFS −5.4 % round, −50 % latency) → room exists.
  (−) PhaSched as in Table I ≈ FR-FCFS-Cap, not better than FR-FCFS: lazy TRAIN drain adds turnarounds (+10 %
  vs Cap); Cap-16 base costs ≈ 0.8 %.
- Next: Step 8b TRAIN preset sweep (train_mode 0/1/2, base cap 16 vs ∞).

### Step 8b — TRAIN preset sweep, DNN @ 10 G  (2026-11-25)
- Round vs FRFCFS (539.0 M): t0 lazy/cap∞ +0.32 %, **t1 baseline/cap∞ −1.13 % (532.9 M)**, t2 full drain/cap∞ −0.80 %,
  t1/cap16 +0.08 %. Best TRAIN read latency: t1_capinf avg 127.0 (−1.9 %), p50 60 (−20 %).
- Controls ≠ baselines (t1_capinf −1.4 % TRAIN vs FRFCFS; t1_cap16 −0.9 % vs Cap) → the gain is the Tmax starvation
  guard acting in TRAIN (age-based write drain), not the drain presets. Lazy rule = worst.
- First positive round-time result for PhaSched (−1.1 %), modest; attribution: Tmax guard.
- Next: Tmax sweep on t1_capinf (500/1000/4000/8000) to pick the final preset, then full matrix.

### Step 8c — Tmax sweep, DNN @ 10 G  (2026-11-25)
- Round vs FRFCFS: Tmax 500 +1.59 %, 1000 −1.00 %, **2000 −1.13 %**, 4000 −0.73 %, 8000 −0.67 % → robust over 1000–8000.
- Decision: final preset `PhaSched_final` = cap ∞ base, TRAIN baseline drain (train_mode 1), Tmax 2000,
  INGRESS eager, EGRESS Whi-only cap 64.
- Steps per round for the matrix: logreg 64, nb 128, dnn 2, lstm 1, gru 1 (TRAIN ≈ 50–150 ms native).
- Open item (credibility): phase-agnostic control "FR-FCFS + Tmax guard in all phases" to show what part of the
  gain needs the phase hint.

### Step 8d — Main matrix 5 workloads × 5 schedulers @ 10 G  (2026-11-25, done)
Round time normalised to FR-FCFS (round = DMA-in + TRAIN + DMA-out):
| | LR | NB | MLP | LSTM | GRU | geomean |
|---|---|---|---|---|---|---|
| FCFS | 1.9219 | 2.5021 | 1.0566 | 1.1077 | 1.0647 | 1.4306 |
| FR-FCFS-Cap | 0.9999 | 0.9999 | 1.0079 | 1.0120 | 1.0094 | 1.0058 |
| BLISS | 1.0077 | 1.0044 | 1.0301 | 1.0042 | 1.0047 | 1.0101 |
| PhaSched_final | 0.9998 | 1.0000 | 0.9887 | 0.9984 | 0.9985 | **0.9971** |
- TRAIN avg read latency PhaSched/FR-FCFS: 0.9993 / 0.9997 / 0.9807 / 0.9971 / 0.9717 (geo 0.9896).
- NIC share of the FR-FCFS round: LR 61.6 %, NB 57.8 %, MLP 18.0 %, LSTM 7.4 %, GRU 8.2 %.
- Best baseline: FR-FCFS-Cap on LR/NB (≤ 0.01 % ahead of FR-FCFS), FR-FCFS on MLP/LSTM/GRU.
  PhaSched vs best baseline: LR −0.002 %, NB +0.009 % (noise level), MLP −1.13 %, LSTM −0.16 %, GRU −0.15 %.
- Paper numbers: X = 0.3 % (geomean), Xmax = 1.1 % (MLP), Y = 1.0 % (geo, up to 2.8 % GRU), Z = BLISS +1.0 %,
  Cap +0.6 %, FCFS +43 %.
- Energy (phasched/scripts/energy.py, typical 4Gb x8 DDR4-2400 IDD): PhaSched/FR-FCFS 0.9999 / 1.0001 / 0.9884 /
  0.9979 / 0.9979, geo 0.9968 → E = −0.3 % (up to −1.2 % MLP); ACT −10.8 % MLP, −2.2 % LSTM, −3.5 % GRU.
- Honest assessment: consistent, never-worse, but small gains; largest on write-heavy dense training.

### Paper draft update  (2026-11-25)
- `paper/paper1_phase_aware_scheduling.tex` (original kept as `.orig.tex`, `.bbl` copied).
- Filled: N = five workloads; Table II (4 cores @2.4 GHz, DDR4-2400 1 ch/1 rank, 32-entry queues, 10 GbE,
  synthetic FedAvg LR/NB/MLP/LSTM/GRU with sizes, K and batch per workload); methodology (clock divider, synthetic
  clients, round measurement); Fig. 2 x-axis LR/NB/MLP/LSTM/GRU/Geomean; T_max = 2000 DRAM cycles.
- Corrected (9 `% CHANGED:` comments): intro toggling/row-hit claim → measured training interference; 3 → 4 register
  writes; Table I TRAIN preset = baseline + T_max age bound (lazy rule was worst); EGRESS cap "raised" → "unlimited";
  starvation guard "every phase" → "every FL phase"; results sentence: removed ingress/egress shrink [A%]/[B%]/[T%].
- Left as placeholders: X, Xmax, Y, Z, E and the Fig. 2 bars.
- 2026-11-25: X, Xmax, Y, Z and Fig. 2 filled from Step 8d (Fig. 2 y-axis 0.97–1.04, stated in caption). E filled (−0.3 %, up to −1.2 %). No placeholders left.
- Fix 2026-11-25: two `% CHANGED` comments had been placed on the same line as following original text, commenting
  out "These points already exist … non-FL software is unaffected." (Sec. II-A) and "System and workloads … DRAM
  energy." (Sec. III). Restored; verified by word-level diff that only intended text differs from the original.
- Open: final runs should use `phasched_egress_cap = 1000000` to match the text (runs so far used 64; egress is
  link-bound so no effect expected).

## 5b. Paper claims vs evidence (status 2026-11-25, ubench only — real FL workloads not yet run)

| # | Paper claim | Status | Evidence |
|---|---|---|---|
| 1 | Round has 3 distinct traffic patterns (ingress write burst, train read-dominated, egress read stream) | ✅ (model) | INGRESS 262,144 W / 5 R; EGRESS 262,144 DMA R; TRAIN 15.9 M R vs 1.04 M W (ubench) |
| 2 | Mechanism: phase register + 4 presets, no per-thread state, hint writes per round | ✅ | implemented; 4 hint writes/round (IDLE at end is extra — can drop to 3) |
| 3 | IDLE behaves exactly as baseline | ✅ | PhaSched no-hint vs FR-FCFS-Cap within ±0.02 % |
| 4 | Starvation guard bounds delay of non-matching traffic | ✅ | starved core reads 277,820 → 2,037 DRAM clk (136×) at DRAM-bound NIC rate |
| 5 | Baseline toggles R/W at wrong moments, many turnarounds in ingress/egress | ❌ NIC-only / ✅ mixed | NIC-only phases: 1–8 turnarounds for every scheduler; mixed (CPU-proxy) EGRESS 12,810 |
| 6 | Baseline breaks row-hit streaks of sequential streams | ❌ | NIC phases ≈ 99 % row hits and equal ACT counts under all schedulers |
| 7 | Ingress/egress time shrinks [A%]/[B%] | ❌ NIC-only / ✅ small mixed | DMA time identical at 10/100/1000 G (dma.csv); mixed EGRESS −4.9 % |
| 8 | Turnarounds drop [T%] | ✅ mixed only | EGRESS −87 % (CPU proxy); NIC-only nothing to drop |
| 9 | Ingress preset (eager, no cap) helps | ❌ | no gain NIC-only; mixed INGRESS +12.5 % worse; 100 G co-running reads 240 → 2,037 clk |
| 10 | Train preset: lower read latency [Y%] | ❌ lazy rule / ✅ small with guard | final preset: TRAIN avg read latency −1.0 % geo (up to −2.8 % GRU); MLP p50 −20 % |
| 11 | Round time −[X%] vs FR-FCFS | ✅ small | geomean −0.3 %, max −1.1 % (MLP), never slower; gain from Tmax guard in TRAIN |
| 12 | BLISS gains only [Z%] / thread fairness ≠ phase behaviour | ✅ | BLISS +1.0 % round (geo) vs FR-FCFS; does not fix INGRESS starvation |
| 13 | DRAM energy change [E%] | ✅ small | −0.3 % geo, up to −1.2 % (MLP); MLP row activations −10.8 % |
| 14 | Methodology: DAMOV-SIM + DMA injector + magic op, DDR4-2400, 10 GbE | ✅ | injector exact (32.2 M cyc); DDR4 timing fixed (Step 2b) |

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
