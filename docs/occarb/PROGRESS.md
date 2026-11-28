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
| 2 | Paper system config (Table II) + baseline workload run | ✅ done |
| 3 | DMA source tagging in Ramulator `Request` (no behaviour change) | ✅ done |
| 4 | NIC model as a standalone class + unit tests | ✅ done |
| 5a | Memory clock fidelity: optional CPU/DRAM clock divider | ✅ done |
| 5b | Integrate NIC injector at the ZSim–Ramulator boundary | ✅ done |
| 6 | Static baselines: Core-first, DMA-first | ✅ done |
| 7 | OCCARB scheduler (3 modes + hysteresis) | ⏳ in progress |
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
- Status: ✅ DONE (run 2)
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
- Run 2 (with the script fixes; build includes the Controller.h edit):
  - 2a PASS: Step 1 deterministic and = 26,587,526. So a given build is reproducible, which
    supports the layout hypothesis.
  - 2b PASS; 2c all PASS: 4 cores, caches, 1200 MHz, queue depth 64, reads 909,128 / writes 385,091.
  - 2d WARN: two identical 4-core runs gave 15,285,537 vs 15,303,697 (**0.119%**). Read/write
    counts also differ slightly from run 1. Multi-threaded ZSim runs are not bit-deterministic:
    host thread scheduling changes the interleaving.
  - 2e PASS: q32 = 14,988,729 vs q64 = 15,285,537. The knob is live.
    - Observation: the 32-entry queue is ~1.9% *faster* than 64 for STREAM ×4. That is ≫ the 0.12%
      noise, so it is a real effect, probably the longer write-drain bursts with a 64-entry
      writeq (watermarks at 0.8/0.2 × max). We keep 64 as specified in Table II.
  - **T_alone (STREAM Copy ×4, 40M instrs, Table II) = 15,285,537 cycles** (±0.12%).
- Methodology decisions from Step 2:
  1. Multi-core noise floor ≈ 0.12%. Every multi-core data point (T_alone and T_overlap) is
     run **3 times and reported as the mean**, with min–max. Effects smaller than ~0.5% are not claimed.
  2. Single-core runs are bit-deterministic within one build, so they serve as exact regression tests.
  3. Code changes shift absolute cycles slightly (layout), so a cross-build regression uses a 0.5%
     tolerance. S = T_overlap/T_alone always uses one build.
- `file ../workloads/STREAM/stream_copy` → `ELF 64-bit LSB shared object ... dynamically linked`,
  so **the binary is PIE**. Its load address (and its static arrays) is chosen at load time, after Pin
  and the tool libraries are mapped. This is consistent with the layout hypothesis: a different
  `libramulator.so` size gives a different app base, so cache sets and DRAM banks shift.
  - Decision: keep the 0.5% cross-build tolerance and same-build S ratios. Recompiling workloads
    with `-no-pie` is a possible later refinement, but not needed for correctness of S.

### Step 3: DMA source tag
- Status: ✅ DONE (run 1)
- Change: `ramulator/Request.h` gets `bool is_dma = false;` right after `int coreid = -1;`.
  The in-class initializer covers all 4 constructors, so the existing core-request paths in
  `src/ramulator_mem_ctrl.cpp` (`tick()`, `enqueue()`) need no edit.
- Checks:
  - 3a: the field is present, and both `libramulator.so` and `libzsim.so` were rebuilt after the edit.
  - 3b: standalone unit test `occarb/step3/test_request.cc`: is_dma is false for all 4 constructors,
    survives a copy, and leaves other fields intact.
  - 3c: Step 1 regression, deterministic and within 0.5% of 26,587,526.
- Pre-check (Claude container): the unit test passes 6/6, and Ramulator compiles with the new field.
- Run 1: **all PASS**.
  - 3a PASS: field present, both libraries rebuilt.
  - 3b PASS: 6/6 unit tests.
  - 3c PASS: Step 1 deterministic and **bit-identical (26,587,526)**. This time the code change did
    not even shift memory layout.

### Step 4: NIC model (standalone)
- Status: ✅ DONE (run 1)
- New `ramulator/NicModel.h`: header-only, no dependencies. Not yet included by the simulator.
- Model (one `tick()` per cycle):
  - Link window: active for `rho·period_cycles` of every `period_cycles`.
  - RX: while active, packets of `pkt_bytes` (1500) arrive at the link rate. A packet that doesn't
    fit is dropped (loss). The buffer drains through 64 B DMA writes; space is freed on completion.
    A partial tail (<64 B) is flushed only when the link is idle.
  - TX: while active, 64 B DMA reads fill the buffer up to S; the link sends a packet each packet-time
    if it is buffered, otherwise it counts an underrun.
  - o = max(RX fill/S, TX emptiness). TX emptiness counts only while active. `level()` = floor(o·2^bits),
    clamped; the paper uses 2 bits.
- Modelling choices to keep in mind for the paper text:
  - RX and TX are each S bytes and each run at the full link rate (full duplex).
  - Loss is counted per packet (tail drop).
  - The TX urgency spike at the start of each window is real behaviour: the buffer is empty when
    the upload starts.
- Unit tests `occarb/step4/test_nic.cc`:
  - 4a: link-rate arithmetic (1/10/25 GbE).
  - 4b: overflow without drain.
  - 4c: zero loss with fast drain.
  - 4d: loss when DRAM bandwidth < link rate.
  - 4e: 2-bit and 3-bit quantisation boundaries.
  - 4f: ρ = 0 / 0.5 / 1.
  - 4g: TX underrun, goodput, and idle-link handling.
  - 4h: o = max(...).
  - 4i: byte conservation.
  - 4j: no re-issue of in-flight bytes, and tail flush.
- Pre-check (Claude container, g++ -Wall -Wextra): 24/24 PASS, no warnings.
- Run 1 (user): **ALL NIC TESTS PASSED** (24/24).

### Step 5a: Memory clock fidelity (found while preparing the NIC integration)
- Status: ✅ DONE
- Finding: `Ramulator::tick()` (`src/ramulator_mem_ctrl.cpp`) returns 1, so it runs every **CPU** cycle
  (2.4 GHz), and DAMOV left the clock divider commented out. Ramulator therefore advances one
  DRAM clock per CPU cycle. DDR4-2400R has tCK = 0.833 ns (1.2 GHz), so the simulated DRAM runs
  **2× too fast**: about 2× the real bandwidth, and DRAM latencies in ns are halved.
  - For OCCARB this matters directly. The paper is about DMA/core contention for DRAM bandwidth;
    doubling the bandwidth would hide most of the slowdown.
- Fix: the divider is now optional through `sys.mem.clockDivide` (default false = DAMOV behaviour).
  `template_occarb.cfg` sets it to **true**. With freqRatio = ceil(2400/1200.5) = 2, DRAM ticks
  every 2nd CPU cycle.
  - Consequence: Step 2's T_alone (15,285,537) was measured with fast DRAM and is superseded by
    the 5a value.
- Checks (`occarb/step5a/run_check.sh`):
  - 5a-1: the default path is unchanged (Step 1 within 0.5% of 26,587,526, log says OFF).
  - 5a-2: 4-core STREAM, OFF vs ON:
    - ON log says "every 2 CPU cycles";
    - CPU cycles / Ramulator `dram_cycles` ≈ 2 (ON) and ≈ 1 (OFF);
    - ON bandwidth ≤ 19.2 GB/s (DDR4-2400 peak);
    - ON is slower than OFF.
- Run 1: **all PASS**.
  - 5a-1: Step 1 = 26,587,526, **bit-identical**; the log says OFF.
  - 5a-2 OFF: cycles 16,577,188; reads 909,127, writes 384,844; dram_cycles 16,577,000 (ratio 1.000); **11.99 GB/s**.
  - 5a-2 ON: cycles 22,159,754; reads 909,103, writes 384,609; dram_cycles 11,079,500 (ratio 2.000); **8.97 GB/s**.
  - With the real DRAM clock, STREAM×4 is **1.337× slower**, with the same request count.
    So 1/3 of the original run time was hidden by the 2× DRAM clock.
  - STREAM×4 reaches 8.97 GB/s, which is 47% of the 19.2 GB/s peak. A 10 GbE NIC (1.25 GB/s per
    direction) adds ~14% (RX only) or ~28% (RX+TX) on top of that.
  - **New T_alone (STREAM Copy ×4, 40M instrs, Table II, clockDivide ON) = 22,159,754 cycles.**
- ⚠ Open question raised by run 1: OFF here (16,577,188) vs Step 2 run 2 (15,285,537) is an **8.4%**
  difference for the same configuration (no divider). Only the build differs, and the DRAM request
  counts are equal to within 0.03%. The within-build noise measured in Step 2 was 0.12%.
  - Either the cross-build layout effect is much larger for multi-core runs, or the within-build
    variance is larger than one pair of runs showed.
  - Check 5a-3 (requested): rerun `occarb/step2/run_check.sh` on the current build. Its 2d runs the
    no-divider 4-core config twice.
- Run 5a-3 (`occarb/step2/run_check.sh` on the current build):
  - Note: the Step 2 script generates its config from `template_occarb.cfg`, which now has
    `clockDivide = true`. So these are **divider-ON** runs, the same config as 5a-2 "on".
  - 2a PASS (Step 1 = 26,587,526); 2b/2c PASS; 2e PASS.
  - **2d: the same build and config gave 24,039,279 vs 21,785,770 cycles (9.4%)**, and 5a-2 "on"
    gave 22,159,754. DRAM requests: 909,028/385,196 vs 909,103/384,609, equal to within 0.15%.
  - **Conclusion: explanation 2.** Within-build run-to-run variance for the 4-thread STREAM run is
    ~10%, not 0.12% (the Step 2 pair was a lucky match). The amount of work is identical, but the
    time is not. This must be fixed before any slowdown can be measured: S for OCCARB is expected
    to be in the 1.0–2.0 range, and the effects between policies may be a few %.
  - Next: diagnose where the variance comes from (per-core instructions/cycles, thread sync in STREAM).
- Diagnosis 1 (per-core zsim.out, both runs):
  - Instructions per core are ≈10.0M in both runs (9.97–10.04M), so the 40M budget is split evenly.
  - All 4 cores report the same cycles within 0.02% (they advance together).
  - **Not** a termination/imbalance effect. Same work, same DRAM request counts, ~10% different time.
  - STREAM is OpenMP (`#pragma omp parallel`, `stream.c:254-268`), and its ROI comes from `zsim_hooks.h`.
  - Hypothesis: the 4 sequential streams share the DRAM banks. Their relative progress (set by
    host-dependent thread start skew) decides how often they evict each other's open rows, i.e.
    the row-hit vs row-conflict mix, which then persists for the whole run.
  - Test: compare the `row_hits` / `row_misses` / `row_conflicts` stats (`ramulator/Memory.h:248-260`)
    of the two runs.
- Diagnosis 2 (row-buffer stats, same build, same config):

  | run | cycles | row_hits | row_misses | row_conflicts |
  |---|---|---|---|---|
  | q64 | 24,039,279 | 951,969 | 17,340 | **324,878** |
  | q64_rep | 21,785,770 | 1,012,657 | 18,648 | **262,406** |

  - **Hypothesis confirmed.** The slower run had 24% more row conflicts (+62k) and 6% fewer row
    hits, for the same total requests (≈1.29M). Run-to-run variance comes from how the 4 threads'
    sequential streams interleave in the shared banks, which is decided by host-dependent thread
    start skew.
  - Relevance to OCCARB: this is the same mechanism DMA streams will trigger (sequential DMA
    streams breaking the cores' open rows), and the reason URGENT serves DMA "in row-hit bursts".
    Row-buffer hit rate should be reported as a supporting metric.
- **Methodology decision (user agreed):**
  - Steps 5b–8 are validated with single-core runs, which are bit-deterministic.
  - Step 9 measures the variance of the real workloads on 4 cores and sets the repeat count.
  - Final results report mean ± spread, and Fig. 2 gets error bars.
  - The risk "variance too large → needs dozens of runs" is deferred. The fallback, if needed,
    is controlled thread start offsets.
  - Paper impact, to write later:
    - §III: one sentence on the DRAM clock fix;
    - §III: one sentence on repeats/variance;
    - §III: the NIC modelling choices;
    - optional row-buffer hit-rate metric;
    - error bars in Fig. 2.

### Step 5b: NIC DMA injector integrated
- Status: ✅ DONE (run 1; one check threshold relaxed, see below)
- Design:
  - The ZSim-side `Ramulator` object owns a `ramulator::NicModel`, ticked from `Ramulator::tick()`
    every CPU cycle (`cycle_hz` = CPU frequency).
  - Each cycle it issues at most 1 RX DMA write and 1 TX DMA read, within `maxInflight` outstanding
    requests per direction.
  - Requests are sequential 64 B lines: the RX ring at 7 GB, the TX source at 7.5 GB, each wrapping
    at 256 MB. They are marked `is_dma = true` and use `coreid = 0`, because Ramulator's per-core
    stat vectors need a valid index.
  - They have their own completion callbacks, so the core-request bookkeeping is untouched.
    A refused send is retried the next cycle.
  - NIC issue runs **after** the core overflow-queue retry in `tick()`, so retried core requests
    are not starved of queue slots.
  - Config: `sys.mem.nic.{enable, bufKB, linkGbps, rho, periodCycles, pktBytes, rx, tx, occBits,
    maxInflight, rxBaseMB, txBaseMB, regionMB}`. All keys are read even when disabled
    (zsim `strictConfig`). Doubles need a decimal point.
  - Stats in `zsim.out` (mem section), only when enabled:
    - `nicCycles`, `nicActiveCycles`
    - `nicRxPktsArrived` / `Dropped` / `BytesAccepted` / `BytesWritten`, `nicRxDmaWrites`, `nicRxDmaLat`, `nicRxSendFails`
    - `nicTxBytesRead` / `BytesSent` / `PktsSent` / `Underruns`, `nicTxDmaReads`, `nicTxDmaLat`, `nicTxSendFails`
    - `nicLevel0..3Cycles`
  - The occupancy level is computed but **not yet used by the scheduler** (Steps 6–7).
- Checks (`occarb/step5b/run_check.sh`, 1-core STREAM, 20M instrs, Table II, clock divider ON):
  - b0: Step 4 tests pass; Step 1 within 0.5%; no nic block → "[NIC] disabled".
  - b1: nic block with `enable = false` → disabled, no NIC stats.
  - b2: enabled with ρ = 0 → zero DMA and **bit-identical cycles** to disabled.
  - b3: RX 10 GbE:
    - nicCycles ≈ core cycles;
    - accepted bytes = 1.25 GB/s × active time (±1%);
    - zero loss;
    - written ≥ accepted − S;
    - the Ramulator write_requests delta equals the DMA writes (±2%);
    - S > 1;
    - deterministic.
  - b4: TX 10 GbE: sends at link rate (±2%); underruns ≤ 1%; the read_requests delta equals the DMA reads.
  - b5: starved engine (maxInflight 1, 16 KB, 25 GbE) → loss > 0.
  - b6: S(RX+TX 25G) > S(RX 10G) > 1.
  - b7: level residency sums to nicCycles.
- Pre-check (Claude container): `src/ramulator_mem_ctrl.cpp` and `src/init.cpp` pass `g++ -fsyntax-only`
  against the real ZSim/Pin headers. `nic_set` was tested on the template.
- Run 1 (1-core STREAM Copy, 20M instrs, Table II, divider ON): 18/19 PASS.
  - b0 PASS.
    - Step 1 = 26,616,590: +0.11% vs 26,587,526, which is the layout shift from the rebuild.
    - **New Step 1 reference for this build: 26,616,590.** The `occarb/step2/run_check.sh` exact
      check (2a) would now fail. It is a historical script; later checks use the 0.5% tolerance.
  - b1 PASS: **T0 (NIC disabled) = 26,241,423 cycles.**
  - b2 PASS: ρ = 0 gives 0 DMA requests and **bit-identical** cycles (26,241,423). Integrating the
    NIC does not perturb the simulator.
  - b3 PASS (except the determinism sub-check): RX 10 GbE.
    - Accepted 14,703,000 B vs expected 14,703,125 B (−0.001%).
    - 0 dropped.
    - The Ramulator write_requests delta (229,738) matches the DMA writes (229,710).
    - nicCycles 28,230,000 ≈ core cycles 28,230,333.
    - Mean DMA write latency 318 CPU cycles (133 ns).
    - **S = 1.0758.**
  - b3 determinism **FAIL by 1 cycle**: 28,230,333 vs 28,230,334, a relative difference of 3.5e-8.
    - Single-core runs *with* DMA are not bit-exact; the cause is unknown, possibly the order of
      events at phase boundaries.
    - Negligible for S (measurement resolution ~1e-6).
    - The check now uses a 1e-6 tolerance.
  - b4 PASS: TX 10 GbE sent 14,502,000 B vs expected 14,502,604 B (−0.004%); 0 underruns; the
    read_requests delta (242,978) matches the DMA reads (242,977); mean DMA read latency 293 cycles;
    S = 27,845,492/26,241,423 = **1.0611**.
  - b5 PASS: starved engine (1 outstanding, 16 KB, 25 GbE) → **76.9% loss**, so the loss path works.
  - b6 PASS: **S(RX+TX 25 GbE) = 1.4415** > S(RX 10 GbE) = 1.0758 > 1. With RX+TX at 25 GbE: 0 loss, 0 underruns.
  - b7 PASS: level residency sums to nicCycles (37,826,000).
- Key observation for OCCARB: under the baseline FR-FCFS with S = 1 MB, the 25 GbE RX+TX run spends
  **99.46% of the time at level 0** (L1/L2/L3 ≈ 0.18% each, the TX buffer filling at the start).
  The buffers have plenty of slack, yet DMA still costs the core 44%.
  - This is exactly the situation OCCARB targets. In RELAXED mode, DMA can be deferred most of the
    time: the buffer absorbs it, and occupancy rises toward θlo/θhi only when needed. Under
    FR-FCFS, DMA competes equally even though it could wait.
  - Expectation for Step 7: OCCARB's slowdown approaches Core-first's while loss stays 0, provided
    S is large enough for the deferral.


### Step 6: Source arbitration policies (FR-FCFS / Core-first / DMA-first)
- Status: ✅ DONE (run 1)
- Design finding: Ramulator serves **one** queue per cycle (`readq`, or `writeq` in write-drain
  mode, or `otherq` for refresh). If DMA writes were mixed into `writeq`, they would trigger write
  drain (writeq ≥ 80%) and block core reads regardless of any priority inside `get_head`.
  - So, as the paper states ("separate logical queues"), DMA requests now go to their own
    **`dmaq`** (reads and writes, depth = queue_size = 64).
  - Each cycle, the head of the core queue (chosen exactly as before) is compared with the head of
    `dmaq` (chosen with the same FR-FCFS-PriorHit `get_head`) by `pick_dma()`.
  - Refresh keeps absolute precedence. Write-drain mode is driven by core writes only.
- Policies (`src_policy` in the Ramulator cfg; default FRFCFS). They map 1:1 onto the paper's modes,
  so Step 7 only has to switch between them:

  | src_policy | rule in `pick_dma` | paper mode |
  |---|---|---|
  | FRFCFS | row hit, then ready, then oldest, across both queues | NEUTRAL |
  | CoreFirst | DMA only if the core candidate cannot issue this cycle | RELAXED |
  | DMAFirst | DMA if it can issue (head is a row hit if any is ready), else core | URGENT |
- Unit tests `occarb/step6/test_sched.cc`. They drive Ramulator through `RamulatorWrapper`, the same
  interface ZSim uses, with hand-built mixes; latency is in Ramulator ticks.
  - 6a: all requests complete; no key == FRFCFS (identical completion times).
  - 6b: CoreFirst puts core first (12/12 of the first completions).
  - 6c: DMAFirst puts DMA first.
  - 6d: FRFCFS is age-based across queues (older DMA first); CoreFirst overtakes older DMA.
  - 6e: DMA *writes* vs core reads: no DMA-triggered write drain under CoreFirst.
  - 6f: core-only and DMA-only traffic are policy-independent (identical completion times).
- Pre-check (Claude container): `libramulator.so` builds; **14/14 unit tests PASS**. Highlights:
  - Simultaneous arrival: core 668 / DMA 1988 ticks under CoreFirst, mirrored under DMAFirst.
  - Older DMA: FR-FCFS core latency 1968 vs CoreFirst 704.
  - DMA writes vs core reads (CoreFirst): core 668 vs DMA 2099.
- ZSim checks (`occarb/step6/run_check.sh`):
  - 6g: regressions (Step 4; Step 1 within 0.5% of 26,616,590).
  - 6h: policy shown in the log.
  - 6i: NIC off gives identical cycles for all policies.
  - 6j: RX+TX 25 GbE ordering S(CoreFirst) ≤ S(FRFCFS) ≤ S(DMAFirst), and DMA latency in the reverse order.
- Note: Step 5b's FR-FCFS numbers (S = 1.4415 at 25 GbE) were measured with DMA mixed into
  readq/writeq. They are superseded by 6j's FRFCFS value.
- User-side build issues before run 1 (logged as lessons):
  1. The Edit-3 line was pasted *over* the `Queue& get_queue(Request::Type type)` function header
     instead of inside `enqueue()`.
  2. Fixed with two pattern-based `sed` commands (delete the stray line; rewrite the original line
     inside `enqueue`) plus re-inserting the `get_queue` header after `/* Member Functions */`.
     Both fixes were first verified on a copy of the file broken the same way.
- Run 1: **all PASS** (the output was cut off in the last line, but it shows PASS).
  - 6a–6f: 14/14 unit tests, the same numbers as the pre-check.
  - 6g: Step 1 = **26,736,521**, +0.45% vs 26,616,590. This is a layout shift from the rebuild,
    within tolerance but near the 0.5% limit. **New Step 1 reference: 26,736,521.**
  - 6h: each policy appears in the log.
  - 6i: NIC off gives identical cycles for all 3 policies. **T0 = 26,306,418** (5b had 26,241,423;
    +0.25%, rebuild layout).
  - 6j (1 core, RX+TX 25 GbE, S = 1 MB):

    | policy | S | RX DMA lat | TX DMA lat | loss | underruns | time at level 3 |
    |---|---|---|---|---|---|---|
    | FRFCFS | 1.4876 | 374 | 481 | 0 | 0 | 0.23% |
    | CoreFirst | **1.4838** | 589 | 827 | 0 | 0 | 0.26% |
    | DMAFirst | 1.4921 | 365 | 469 | 0 | 0 | 0.22% |

    - The ordering is correct: S is CoreFirst < FRFCFS < DMAFirst, and DMA latency is
      DMAFirst < FRFCFS < CoreFirst.
    - **But the policies differ by only 0.6% in S, while the DMA cost itself is 48%.**
- Analysis of the small policy spread (important for the evaluation design):
  1. DMA must eventually be served at the link rate under every policy, so its total bandwidth is the
     same. A priority policy can only change *when* DMA is served, and which core requests it delays.
  2. With **1 core**, the core rarely has a ready request every cycle (limited memory-level
     parallelism). So even CoreFirst finds plenty of idle slots for DMA: the DMA latency is only
     589 vs 374 cycles, and the buffers never fill (level 3 ≈ 0.25%, no loss).
  3. The 48% slowdown therefore comes mostly from interference that priority doesn't remove: DMA
     row activations closing the core's open rows, read/write bus turnarounds, and shared bandwidth.
  4. The paper's claims (Core-first loses packets, OCCARB ≈ Core-first slowdown with no loss) need
     a **memory-saturating** core workload, so that CoreFirst actually starves DMA and buffer
     occupancy rises. With 1 core that never happens.
  - Consequence for Step 7: validate OCCARB's mechanics on 1 core (deterministic), and add a
    **4-core saturating stress test** (STREAM ×4 + 25 GbE, 3 repeats). There, CoreFirst should
    show loss > 0 and OCCARB should show URGENT residency > 0 with ~0 loss.
  - Possible refinement, to evaluate in Step 7: in RELAXED mode, the paper says DMA may only go to
    "banks with no ready core request". Our CoreFirst uses "the core head can't issue this cycle".
    A stricter variant (no DMA to a bank with *any* queued core request) would also reduce the
    row-conflict interference from point 3.

### Step 7: OCCARB (occupancy-driven arbitration)
- Status: in progress
- Design:
  - **Sideband.** Each CPU cycle the ZSim-side NIC sends `(level, 2^occBits)` through
    `RamulatorWrapper::set_io_level()` → `MemoryBase::set_io_level()` (virtual, default no-op, so HMC
    is unaffected) → each `Controller`. The controller uses o = level / levels, the paper's quantised
    2-bit level.
  - **Fallback** (`occarb_source = dmaq`, the paper's "own DMA queue as proxy"):
    o = floor(dmaq.size · 2^bits / dmaq.max) / 2^bits.
  - **State machine** `occ_update()`, once per controller cycle, before arbitration:
    - URGENT → RELAXED only when o < θlo (hysteresis);
    - otherwise o ≥ θhi → URGENT, o ≥ θlo → NEUTRAL, else RELAXED.
    - RELAXED can jump straight to URGENT if o jumps.
  - **Mode → service order** (reuses Step 6): RELAXED = CoreFirst, NEUTRAL = FR-FCFS, URGENT = DMAFirst.
  - **Option `relaxed_strict = on`:** in RELAXED/CoreFirst, DMA is also held back from any bank with
    a queued core request (read or write queue). This goes beyond the paper's wording ("banks with
    no ready core request"); kept as an option, off by default.
  - **Ramulator cfg keys:** `src_policy = OCCARB`, `occarb_theta_lo`, `occarb_theta_hi` (fractions of
    the buffer; default 0.25 / 0.75), `occarb_source` (nic|dmaq), `occarb_occ_bits` (dmaq only),
    `relaxed_strict`.
  - **Stats** (`.ramulator.stats`, OCCARB only): `occarb_{relaxed,neutral,urgent}_cycles_<ch>`,
    `occarb_transitions_<ch>`.
  - Hardware cost as in the paper: two comparisons, a 2-bit mode register, and the `is_dma` bit (Step 3).
- Delivery: because of the manual-paste problems in Steps 2 and 6, the source edits come as a
  **unified diff for GNU `patch`**, `occarb/step7/step7.patch` (186 lines, no tabs).
  - It is applied with `patch -p1 -N -l --dry-run`, then without `--dry-run`. `patch` checks the
    context lines, `-N` skips a patch that was already applied, and `-l` tolerates whitespace damage
    from copy-paste.
  - Tested on the pre-Step-7 files, including a copy with all trailing whitespace stripped: the
    results are byte-identical to the reference, and a second application is detected and skipped.
  - (A first version used a Python patch script. **The user's machine cannot use Python**; the
    script was removed. Rule: deliver only bash / standard Unix tools, never Python.)
- Unit tests `occarb/step7/test_occarb.cc`, through RamulatorWrapper: **14/14 PASS** in the Claude container.
  - 7a: 12-step level sequence follows Table I incl. hysteresis; 7 transitions; residency sums to cycles.
  - 7b: degenerate thresholds equal the static policies **exactly** (identical completion times):
    lo = hi = 1 → CoreFirst; lo = 0, hi = 1 → FR-FCFS; lo = hi = 0 → DMAFirst.
  - 7c: dmaq source: a full DMA queue → URGENT (305 cycles), back to RELAXED when drained;
    nic source with no sideband → stays RELAXED.
  - 7d: strict == CoreFirst when no bank is shared. In a targeted scenario (bank found by asking
    Ramulator's own address mapping) strict holds DMA back 747 times; bank-X core writes finish
    sooner (1093 vs 1135 ticks); no starvation.
    - The first version of 7d (workload A) could not trigger the strict case at all; it was replaced
      with the targeted scenario.
- ZSim checks (`occarb/step7/run_check.sh`, 1 core, 25 GbE RX+TX):
  - 7e: regressions (Step 4 and Step 6 unit tests; Step 1 within 0.5% of 26,736,521).
  - 7f: parameters in the log.
  - 7g: OCCARB(lo = hi = 1) == CoreFirst and OCCARB(lo = hi = 0) == DMAFirst in ZSim (±1e-6). This
    proves the sideband plumbing and the mode mapping end-to-end.
  - 7h: residency sums to DRAM cycles; with ρ = 0.5 the sideband drives transitions incl. URGENT.
  - 7i: S(CoreFirst) ≤ S(OCCARB) ≤ S(DMAFirst); OCCARB has zero loss.
- Stress experiment (`occarb/step7/stress.sh [REPEATS] [BUF_KB]`, default 3 × 256 KB). Reports
  findings, not pass/fail: 4 cores STREAM, 25 GbE RX+TX; FR-FCFS / CoreFirst / DMAFirst / OCCARB;
  S mean [min, max], loss, underruns, URGENT %.
- User-side apply: hunk 1 of the patch failed at first. The user's Step 6 paste had 2 extra blank
  lines after `long dma_cmds ...` (seen with `cat -A`). Deleting lines 187–188 let the patch apply.
  This was verified first on a reproduction of the user's file.
- Run 1 (`run_check.sh`):
  - 7a–7d: **14/14 unit tests PASS** on the user's build (identical numbers to the pre-check).
  - 7e: Step 4/6 unit tests PASS. **Step 1 FAIL**: its two internal runs differed
    (26,638,421 vs 26,573,526, 0.24%). This is the first non-repeatable Step 1 since the start.
  - 7f PASS.
  - **7g FAIL**: OCCARB(θ = 1) 38,634,490 vs CoreFirst 39,136,565 (−1.3%); OCCARB(θ = 0) 39,244,762
    vs DMAFirst 39,265,750 (−0.05%). The unit tests prove these are decision-identical.
  - 7h PASS: oc RELAXED 19,285,249 / URGENT 132,251 / 2 transitions; oc_win 4 transitions incl. URGENT;
    residency sums exactly to dram_cycles.
  - **7i FAIL**: S = CF 1.4937, FR 1.4944, OC 1.4823, DF 1.4987. OC falls below CF, by an amount
    comparable to the oc_R anomaly. Loss 0 everywhere.
- Probe 1 (repeat runs on the same build):
  - Step 1 (no NIC): 26,638,421 ×3 (but earlier 26,573,526 once).
  - CoreFirst + NIC: 39,136,565 ×3 (repeatable).
  - **OCCARB(θ = 1): 38,634,490 then 39,100,017 (1.2% apart, same config)**.
  - The config diff is only `src_policy` + the θ lines.
  - Conclusion: **intermittent run-to-run non-determinism in the simulation itself**, not an OCCARB
    scheduling difference. It appears even without the NIC (Step 1). The earlier 1-cycle rerun
    difference in 5b was an early sign.
- Hypothesis: STREAM's first `#pragma omp parallel` (`stream.c:254-259`) runs before
  `omp_set_num_threads(n)`, so libgomp starts **as many threads as the host has CPUs**, even for
  `stream_copy 1`. Idle pool threads block in futex waits. ZSim handles blocking syscalls with a
  watchdog on host time, so when idle threads wake and are scheduled onto the simulated core can
  depend on host timing. This could also contribute to the ~10% 4-core spread (Step 5a-3), where
  barrier waits are real.
- Probe 2 (`occarb/step7/probe_env.sh`, no code change): adds
  `env = "OMP_NUM_THREADS=<cores> OMP_WAIT_POLICY=ACTIVE"` to the process (ZSim supports
  `processN.env`, `src/pin_cmd.cpp:120-179`) and repeats:
  - A: Step 1 ×3;
  - B: OCCARB(θ = 1) ×2 + CoreFirst ×2;
  - C: 4-core STREAM ×3 (also prints row_conflicts).
- Run 2: _(pending)_- Probe 2 results (env `OMP_NUM_THREADS=<cores> OMP_WAIT_POLICY=ACTIVE`):
  - A: Step 1 = 26,459,523 ×3, repeatable.
  - B (1 core + NIC 25 GbE): CoreFirst 38,506,528 / 38,934,632; OCCARB(θ = 1) 38,833,419 / 39,013,505.
    **Still ~1.1% run-to-run.**
  - C (4 cores, no NIC): 26,146,815 / 22,242,802 / 22,723,863, with row_conflicts
    511,128 / 280,681 / 292,467. **Still ~15%.**
  - **The OpenMP-pool hypothesis is rejected.** The runs that vary all have several sequential streams
    sharing banks (4 STREAM threads, or core + DMA streams). Their relative alignment sets the
    row-conflict rate, so tiny timing perturbations are amplified into 1–15% swings.
  - Ramulator uses the `RoBaRaCoCh` mapping (`Memory.h:96`): consecutive lines fill a row, then
    move to the next bank. There is no XOR bank hashing, which real controllers use to break
    exactly this kind of stream alignment.
- Decisions:
  1. Keep `env = "OMP_NUM_THREADS=<cores> OMP_WAIT_POLICY=ACTIVE"` in `template_occarb.cfg` and the
     Step 1 smoke config (correct thread count, avoids host oversubscription; Step 1 repeatable 3/3).
     **New Step 1 reference: 26,459,523.**
  2. Runs with DMA, and 4-core runs, are treated as noisy measurements. At the ZSim level, 7g/7i use a
     2% band (measured 1-core noise ~1.1%). Exact equivalence of OCCARB with the static policies is
     proven where the simulation is deterministic, in the Ramulator unit tests (7b).
  3. The 4-core stress test (3 repeats, min/max) measures effect size vs noise. If policy differences
     (especially loss) do not clearly exceed the spread, try longer runs and/or XOR bank hashing
     before the Step 10 sweeps. Paper impact: report mean ± range (already planned).
- Edits (applied by the user with `sed`, no Python):
  - template `env` line;
  - smoke.cfg `env` line;
  - step7/run_check.sh: REF1 → 26,459,523, 7g/7i tolerance 2%.
- Run 2: _(pending)_
