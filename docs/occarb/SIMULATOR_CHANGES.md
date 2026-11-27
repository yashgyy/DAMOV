# DAMOV-SIM Change Log (OCCARB)

Every change made to the simulator (ZSim, Ramulator, configs, scripts) for OCCARB.
For each change: what file, what changed, why, and how to undo it.

Base: unmodified DAMOV repository (commit `7a2147b`, "Update get_stats_per_app.py").

Legend: **[NEW]** new file · **[MOD]** modified existing file · **[DEL]** deleted file

---

## Summary of touched files

| File | Type | Step | Purpose |
|---|---|---|---|
| `simulator/occarb/step1/smoke.cfg` | NEW | 1 | Smoke-test config: 1 core + Ramulator DDR4 |
| `simulator/occarb/step1/run_check.sh` | NEW | 1 | Automated Step 1 checks (1a–1d) |

Existing simulator source files modified so far: **none**.

---

## Step 1: Baseline build + smoke run

### What we did
- Built the unmodified simulator (`scripts/compile.sh`: libconfig → `libramulator.so` → ZSim via scons).
- Added a smoke test to confirm that ZSim drives Ramulator DDR4 and that results are repeatable.

### Changes
- **[NEW] `simulator/occarb/step1/smoke.cfg`**
  - 1 `Simple` core, 32 kB L1i/L1d, 256 kB L2, `mem.type = "Ramulator"` using the
    existing `ramulator-configs/DDR4-config.cfg`.
  - Workload: `dd if=/dev/zero of=/dev/null bs=4M count=100`. The 4 MB buffer is bigger
    than the L2, so the traffic must reach DRAM.
  - `sim.stats = "smoke"`, so Ramulator writes `smoke.ramulator.stats` to the run directory.
- **[NEW] `simulator/occarb/step1/run_check.sh`**
  - Runs the smoke config twice, in `occarb/step1/out_run1` and `out_run2`. It symlinks
    `ramulator-configs/` into each run directory, because the config path is relative.
  - Checks:
    - 1a: build outputs exist.
    - 1b: the run exits 0 and writes `smoke.zsim.out`.
    - 1c: core cycles > 0 and Ramulator `read_requests` > 0 (originally `numberofreads_s_0`, which is a DRAMPower-only stat).
    - 1d: the cycle count is identical across the two runs.

- **[MOD, our own file] `simulator/occarb/step1/run_check.sh`** (after run 1):
  - It now reads `smoke.zsim.out` instead of `zsim.out`.

- **[MOD, our own files] `smoke.cfg` + `run_check.sh`** (after run 2, Pin injector crash):
  - `smoke.cfg` now mirrors `templates/template_host_ooo.cfg`: OOO core, `pinOptions = "-ifeellucky"`,
    `gmMBytes = 8192`, `deadlockDetection = false`, `startFastForwarded = True`. The workload is
    `WORKLOADS_ROOT/STREAM/stream_copy 1` instead of `dd`.
  - `run_check.sh` replaces `WORKLOADS_ROOT` with `<repo>/workloads` and writes the result into each run directory.

### Ramulator stats to use (lesson from Step 1)
- Request counts: `read_requests`, `write_requests`, `incoming_requests` (`ramulator/Memory.h:176-199`).
- `numberof*_s_<ch>` (`Controller.h:414+`) are DRAMPower energy counters. Don't use them for traffic.

### Required ZSim settings on this setup (lesson from Step 1)
- `sim.pinOptions = "-ifeellucky"` is **mandatory**. Without it, Pin 2.14 aborts
  (`Injector exited with signal 6`) and the harness still exits 0.
- Use workload binaries from `workloads/`. Don't use system binaries.
- Every OCCARB config will be derived from DAMOV's templates to inherit these settings.

### DAMOV output-file naming (important for all later scripts)
DAMOV changed ZSim's output naming. Every output goes to the run's working directory
(`getcwd`, `src/zsim_harness.cpp:332`) with the `sim.stats` value as a prefix:
- `<stats>.zsim.out`, `<stats>.zsim.h5`, `<stats>.zsim-ev.h5`, `<stats>.zsim-cmp.h5` (`src/init.cpp:842-845`)
- `<stats>.ramulator.stats` (`src/ramulator_mem_ctrl.cpp:105`)

### Simulator behaviour changed?
No. Only new files were added.

### Undo
`rm -rf simulator/occarb/step1`

### Observations for later steps (no change made yet)
- `ramulator/Controller.h:170`: `Queue::max = 32`. Table II needs 64 (Step 2).
- `ramulator/Scheduler.h:28`: only FCFS, FRFCFS, FRFCFS_Cap and FRFCFS_PriorHit exist.
  Core-first, DMA-first, OCCARB and BLISS must be added (Steps 6–8).
- `ramulator/Request.h:22`: requests carry only `coreid`. There is no source (DMA/core) tag yet (Step 3).
- `src/ramulator_mem_ctrl.cpp:161`: `Ramulator::tick()` is where the NIC/DMA injector will hook in (Step 5).
- `templates/template_host_ooo.cfg:68`: uses the HMC config, not DDR4 (Step 2).
