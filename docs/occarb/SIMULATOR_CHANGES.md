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
    - 1b: the run exits 0 and writes `zsim.out`.
    - 1c: core cycles > 0 and Ramulator `numberofreads_s_0` > 0.
    - 1d: the cycle count is identical across the two runs.

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
