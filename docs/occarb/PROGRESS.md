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
| 5b | Integrate NIC injector at the ZSim–Ramulator boundary | ⏳ in progress |
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
- Status: in progress
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
- Run 1: _(pending)_
