# OCCARB Implementation Log

Implementation of *"Occupancy-Driven I/O–Core Memory Arbitration for Asynchronous
Federated Learning"* (OCCARB) on top of DAMOV-SIM (ZSim + Ramulator).

We take the work **one step at a time**. A step counts as done only when all of its
verification checks pass. Each check's result is recorded in the log below.

Simulator code/config changes are tracked separately in
[`SIMULATOR_CHANGES.md`](SIMULATOR_CHANGES.md).

---

## Target results (placeholders in the paper)

| Symbol | Meaning | Value |
|---|---|---|
| [S] | NIC buffer size (MB) | TBD |
| [range] | θlo/θhi sweep range | TBD |
| [A] | FR-FCFS slowdown, ρ=1, 10 GbE | TBD |
| [A'] | DMA-first slowdown, ρ=1, 10 GbE | TBD |
| [D] | Core-first slowdown, ρ=1, 10 GbE | TBD |
| [L%] | Core-first packet loss | TBD |
| [B] | OCCARB slowdown | TBD |
| [C%] | OCCARB packet loss | TBD |
| [U%] | OCCARB time in URGENT | TBD |
| [x], [y] | best θlo, θhi (fraction of buffer) | TBD |
| Fig. 2 | slowdown vs ρ at 10 GbE, 4 policies | TBD |

---

## Step plan

| # | Step | Status |
|---|---|---|
| 1 | Baseline build + smoke run (unmodified DAMOV-SIM) | ⏳ in progress |
| 2 | Paper system config (Table II) + baseline workload run | ☐ |
| 3 | DMA source tagging in Ramulator `Request` (no behaviour change) | ☐ |
| 4 | NIC model as a standalone class + unit tests | ☐ |
| 5 | Integrate NIC injector at the ZSim–Ramulator boundary | ☐ |
| 6 | Static baselines: Core-first, DMA-first | ☐ |
| 7 | OCCARB scheduler (3 modes + hysteresis) | ☐ |
| 8 | BLISS baseline | ☐ |
| 9 | Workloads: ResNet-18 / MobileNetV2 training ROI | ☐ |
| 10 | Experiment sweep scripts | ☐ |
| 11 | Analysis, Fig. 2, fill placeholders | ☐ |

### Step details and verification checks

**Step 1: Baseline build + smoke run**
- Build libconfig, Ramulator (`libramulator.so`), and ZSim with `scripts/compile.sh`.
- Run a small known program through ZSim with Ramulator DDR4 as main memory.
- Checks:
  - 1a: `build/opt/zsim` exists, and `ramulator/libramulator.so` exists.
  - 1b: the smoke run exits 0 and writes `zsim.out` / `zsim-ev.h5`.
  - 1c: `zsim.out` shows non-zero instructions/cycles for the cores, and the
    Ramulator stats show non-zero reads (memory traffic really reaches Ramulator).
  - 1d: rerunning gives identical cycle counts (determinism, needed for S = T_overlap/T_alone).

**Step 2: Table II config**
- New `templates/template_occarb.cfg`: 4 OoO cores; 32 kB L1, 256 kB L2, 8 MB shared L3;
  main memory = Ramulator with `ramulator-configs/DDR4-occarb.cfg` (DDR4-2400, 1 channel).
- Ramulator queue depth 32 → 64 (`Controller.h`, `Queue::max`), made configurable.
- Checks: the config is echoed correctly in `zsim.out` (4 cores, cache sizes, DDR4);
  queue size = 64 is printed or asserted; a baseline workload completes; record T_alone.

**Step 3: DMA source tag**
- Add `bool is_dma` (or a source enum) to `ramulator::Request`; tag core requests = false.
- Checks: the code compiles; the Step 2 run gives **bit-identical** cycles/stats (no behaviour change).

**Step 4: NIC model (standalone)**
- `NicModel` class: RX/TX buffers of S bytes, link rate → bytes/memory-cycle,
  overlap duty cycle ρ, occupancy o = max(RX fill, TX emptiness) quantised to 2 bits,
  overflow → loss counter, goodput counter.
- Checks (unit tests, no simulator): fill-rate arithmetic; overflow when not drained;
  zero loss when drained faster than the link rate; quantisation boundaries; ρ=0 → no traffic,
  ρ=1 → always active, ρ=0.5 → ~50% active.

**Step 5: Integrate the injector**
- `Ramulator::tick()` asks `NicModel` for DMA requests and sends them as cache-bypassing
  sequential 64 B writes (RX) and reads (TX), with `is_dma=true`; completions drain the buffers.
- Checks: ρ=0 → identical to Step 3; 10 GbE, large buffer → measured DMA bandwidth ≈ 1.25 GB/s
  and zero loss; tiny buffer → losses > 0; training slowdown under FR-FCFS > 1.

**Step 6: Core-first / DMA-first**
- Checks: scheduler unit tests on synthetic queues; integration: DMA-first → ~0 loss,
  highest slowdown; Core-first → lowest slowdown, losses > 0 at high load.

**Step 7: OCCARB**
- Checks: state-machine unit tests (RELAXED→NEUTRAL at θlo, →URGENT at θhi, stays URGENT
  until o<θlo); in RELAXED, DMA is issued only to banks without a ready core request;
  in URGENT, DMA is served in row-hit order; mode residency sums to 100%;
  θlo=θhi=max → behaves like Core-first; θlo=θhi=0 → behaves like DMA-first.

**Step 8: BLISS**
- Checks: blacklisting unit test; matches the qualitative BLISS behaviour on a core-only run.

**Step 9: Workloads**
- ResNet-18 and MobileNetV2 Darknet cfgs, training ROI marked for ZSim.
- Checks: runs natively; runs under ZSim; ROI instruction count is stable.

**Step 10: Sweeps**
- Policies × ρ ∈ {0,…,1} × {1,10,25} GbE × workloads; θ sweep for OCCARB.
- Checks: a dry run lists the expected job count; a 2-job mini sweep finishes and parses.

**Step 11: Analysis**
- Parser → CSV → Fig. 2 + placeholder table above.

---

## Log

### Step 1: Baseline build + smoke run
- Status: in progress
- Notes (repo state before starting):
  - `ramulator/Controller.h` queue depth is 32 (paper needs 64): handled in Step 2.
  - `templates/template_host_ooo.cfg` uses the HMC config: Step 2 switches it to DDR4.
  - `./workloads` is not downloaded yet: needed only by Step 2 / Step 9.
- Files added:
  - `simulator/occarb/step1/smoke.cfg`: 1 core, 256 kB L2, Ramulator DDR4. Runs
    `dd bs=4M`, so the traffic is bigger than the L2 and must reach DRAM.
  - `simulator/occarb/step1/run_check.sh`: runs the smoke test twice and checks 1a–1d.
- How to run (from `simulator/`):
  1. `bash scripts/compile.sh` (if not built yet)
  2. `bash occarb/step1/run_check.sh`
- Results: _(paste the PASS/FAIL output here)_
