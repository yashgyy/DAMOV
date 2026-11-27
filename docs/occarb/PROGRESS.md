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
| 1 | Baseline build + smoke run (unmodified DAMOV-SIM) | ✅ done |
| 2 | Paper system config (Table II) + baseline workload run | ⏳ in progress |
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
  - 1b: the smoke run exits 0 and writes `<sim.stats>.zsim.out` (DAMOV naming).
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
- Status: ✅ DONE (run 4)
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
- Run 1 (2026-11-27):
  - Build OK: libconfig installed, `libramulator.so` up to date, ZSim `build/opt` up to date.
  - 1a PASS (both build outputs present).
  - 1b/1c/1d FAIL, but ZSim exited with rc=0. **Cause: a bug in the check script, not in the
    simulator.** DAMOV names its output `<outputDir>/<sim.stats>.zsim.out`
    (`src/init.cpp:845`), here `smoke.zsim.out`, not the stock ZSim name `zsim.out`.
  - Fix: `run_check.sh` now reads `smoke.zsim.out` (2 lines changed).
- Run 2 (after the script fix):
  - 1a PASS. 1b/1c/1d FAIL: no `smoke.zsim.out` was written.
  - `zsim.log` ends with `C: Injector exited with signal 6` / `E: Wait for injector failed`.
    Pin 2.14 aborted while injecting into the process, so the target never ran.
    ZSim's harness still exits 0, which is why rc=0.
  - On kernels >= 4.x, ZSim already forces `-injection parent` (`src/pin_cmd.cpp:71-77`).
  - Suspected causes (being diagnosed):
    - ptrace blocked (Docker without `SYS_PTRACE`/seccomp, or Yama `ptrace_scope` != 0);
    - Pin 2.14 failing on the host's modern `dd` binary/glibc.
  - Diagnostics requested: kernel/OS, ptrace_scope, bare Pin on `/bin/ls`, stock `tests/simple.cfg`.
- Diagnosis (user ran the stock DAMOV flow): STREAM Copy compiled with `workloads/STREAM/compile.py`;
  configs generated with `scripts/generate_config_files.py command_files/stream_cf`;
  `host_ooo/no_prefetch/stream/1/Copy_Copy.cfg` with maxTotalInstrs=1M → **ran fine**
  (Ramulator initialised, STREAM started, clean termination). So Pin and ptrace work.
- Root cause: the smoke cfg was missing the settings DAMOV's template sets:
  `sim.pinOptions = "-ifeellucky"` (Pin 2.14 kernel-version bypass) and `gmMBytes = 8192`.
  The system `dd` binary was also swapped for the known-good STREAM binary.
- Fix: rewrote `smoke.cfg` (1 OOO core, DDR4, STREAM Copy, 20M instrs, `-ifeellucky`,
  `startFastForwarded`) and `run_check.sh` (substitutes the workloads path, checks the binary
  exists, detects "Injector exited").
- Run 3:
  - 1a PASS (zsim, libramulator.so, stream_copy).
  - 1b PASS: `smoke.zsim.out` written.
  - 1c: core cycles = **26,637,522**, PASS. Ramulator-reads check FAIL. The stat
    `numberofreads_s_0` doesn't appear in the output; it is a DRAMPower energy counter
    (`Controller.h:424`), not a general one.
  - 1d PASS: deterministic (26,637,522 in both runs). Repeated runs of the same config are
    therefore exactly reproducible, so S = T_overlap/T_alone needs no averaging.
  - Fix: check the `read_requests` / `write_requests` stats (`Memory.h:182-188`) instead;
    on failure the script prints the head of the stats file.
- Run 4: **all PASS**.
  - 1a PASS: zsim, libramulator.so, stream_copy all present.
  - 1b PASS: `smoke.zsim.out` written.
  - 1c PASS: core cycles = 26,637,522; Ramulator read_requests = 454,655, write_requests = 225,328.
    The read:write ratio of about 2:1 is what STREAM Copy should give: it reads `a`, reads `c`
    on each write-allocate miss, then writes `c` back.
  - 1d PASS: deterministic (26,637,522 == 26,637,522).
  - **Reference value: Step 1 smoke = 26,637,522 cycles.** Used as a regression check in later steps.

### Step 2: Table II config
- Status: in progress
- Files: see SIMULATOR_CHANGES.md § Step 2.
  - New: `ramulator-configs/DDR4-occarb.cfg`, `templates/template_occarb.cfg`,
    `occarb/rebuild.sh`, `occarb/lib.sh`, `occarb/step2/run_check.sh`.
  - Modified: `ramulator/Controller.h`, which adds the `queue_size` knob.
- Workload for the baseline: STREAM Copy with 4 threads on 4 cores, 40M total instructions.
  ResNet-18/MobileNetV2 come in Step 9.
- Execution time T = max over cores of `cycles` in `<stats>.zsim.out`.
- Checks:
  - 2a: Step 1 regression (cycles still 26,637,522).
  - 2b: the Table II run completes.
  - 2c: config applied (4 cores, cache sizes, mem freq 1200 MHz, queue depth 64, DRAM traffic).
  - 2d: the 4-core run is deterministic.
  - 2e: `queue_size` 32 vs 64 changes the timing (the knob is live).
- Pre-check: Ramulator with the edit compiles cleanly (verified in a scratch copy; the container's newer
  GCC needs `-include cstdint` there, which is pre-existing and not needed on the user's GCC).
- Rebuild attempt 1: FAILED to compile (`Controller.h:191: expected unqualified-id before 'if'`).
  - Cause: the edit was pasted at class scope, after the member declaration `bool with_drampower = false;`
    (line ~189), instead of inside the constructor. The anchor text appears twice in the file.
  - Fix: move the 6 lines into the constructor, right after the call `fake_ideal_DRAM(configs);` (~line 230).
  - Lesson: from now on, every edit gives anchor context that is unique in the file, plus a grep to verify placement.
- Rebuild attempt 2: OK (after moving the edit into the constructor).
- Run 1:
  - 2b PASS; 2c all PASS. The run shows 4 cores, L1d/L1i 32 kB, L2 256 kB, L3 8 MB,
    mem freq 1200 MHz, queue depth 64, reads = 909,166, writes = 384,896.
  - 2d/2e/T_alone read 0. **This was a script bug**: `max_cycles` used `[[:space:]]` inside awk,
    and this system's awk is **mawk**, which doesn't support POSIX character classes. 2d
    "passed" as 0 == 0, which was a false pass.
    - Fix: the matching is now done by grep (`lib.sh`); 2d fails if cycles parse as 0.
  - 2a FAIL: Step 1 = **26,587,526** cycles after the rebuild vs 26,637,522 before (−0.19%).
    - Hypothesis: this is not a behaviour change. Without `queue_size`, the edit only adds a
      `printf`. The difference comes from the rebuild itself: the earlier `libramulator.so` was a
      pre-existing build ("up to date"), and a new tool binary shifts Pin/app memory layout, so
      physical addresses and DRAM bank mapping change.
    - Verification requested (2a-revert): remove the 6 lines, rebuild, rerun Step 1. If that also
      gives 26,587,526, the edit is behaviour-neutral, and 26,587,526 becomes the reference.
  - Lesson: compare cycles only within one build. Every rebuild re-baselines T_alone, and
    S = T_overlap/T_alone always uses the same binary for both runs.
- 2a-revert test (6 lines removed → rebuild → Step 1 → edit restored → rebuild):
  - Without the edit, the result is **26,637,522**, the original value. With the edit, it is 26,587,526.
  - **The hypothesis "the rebuild itself shifted the timing" is refused.** The difference
    follows the edit.
  - Code inspection: Step 1 uses `DDR4-config.cfg`, which has no `queue_size` key, and
    `Config::operator[]` returns "" for missing keys (`Config.h:35-40`). So the `if` body
    never runs, and the only executed addition is a `printf` to stdout. Neither can change
    scheduling logic.
  - New hypothesis: **memory-layout sensitivity.** Pin loads `libzsim.so`/`libramulator.so` into
    the application's address space. Changing the library's size moves where the (possibly PIE)
    application and its arrays are placed. ZSim uses virtual line addresses directly, so
    cache-set and DRAM bank/row mapping shift, and so do the cycles. If this is right, any
    code change (including every later OCCARB step) will shift absolute cycles slightly.
  - Next check: is `stream_copy` PIE (`file`)? Also run Step 2 with the restored build.
- Run 2: _(pending)_
