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

| `simulator/ramulator-configs/DDR4-occarb.cfg` | NEW | 2 | DDR4-2400, 1 ch, 1 rank, 8Gb x8, `queue_size = 64` |
| `simulator/templates/template_occarb.cfg` | NEW | 2 | Table II system template (4 OOO cores, L1/L2/L3, Ramulator) |
| `simulator/ramulator/Controller.h` | **MOD** | 2 | `queue_size` config knob for readq/writeq depth + print |
| `simulator/occarb/rebuild.sh` | NEW | 2 | Clean rebuild of Ramulator + ZSim |
| `simulator/occarb/lib.sh` | NEW | 2 | Shared helpers for check scripts |
| `simulator/occarb/step2/run_check.sh` | NEW | 2 | Step 2 checks (2a–2e) |

| `simulator/ramulator/Request.h` | **MOD** | 3 | `bool is_dma = false;` source tag |
| `simulator/occarb/step3/test_request.cc` | NEW | 3 | Unit test for the tag |
| `simulator/occarb/step3/run_check.sh` | NEW | 3 | Step 3 checks (3a–3c) |

| `simulator/ramulator/NicModel.h` | NEW | 4 | NIC RX/TX buffer + link + occupancy model (header-only) |
| `simulator/occarb/step4/test_nic.cc` | NEW | 4 | NIC unit tests (4a–4j) |
| `simulator/occarb/step4/run_check.sh` | NEW | 4 | Builds and runs the NIC unit tests |

Existing simulator source files modified so far: `ramulator/Controller.h` (Step 2), `ramulator/Request.h` (Step 3).

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

---

## Step 2: Table II system config

### What we did
- Defined the paper's system (Table II) as a DAMOV-style template plus a dedicated DDR4 config.
- Made the Ramulator controller queue depth configurable, because the paper needs 64 entries
  and the stock value is a hard-coded 32.

### Changes
- **[NEW] `simulator/ramulator-configs/DDR4-occarb.cfg`**
  - Copy of `DDR4-config.cfg` with `org = DDR4_8Gb_x8` (8 GB, 1 rank) and a new line
    `queue_size = 64`.
  - Unchanged: `standard = DDR4`, `channels = 1`, `ranks = 1`, `speed = DDR4_2400R`.
  - The Ramulator config parser takes exactly 2 tokens per line, so inline comments are not allowed.
- **[NEW] `simulator/templates/template_occarb.cfg`**
  - Derived from `template_host_ooo.cfg`: OOO cores; L1d 32 kB/8-way/4 cyc; L1i 32 kB/4-way/3 cyc;
    L2 256 kB/8-way/7 cyc private; L3 8 MB/16-way/16 banks/27 cyc shared; Ramulator memory;
    `-ifeellucky`; `gmMBytes = 8192`; `startFastForwarded = True`.
  - Placeholders: `NUMBER_CORES`, `STATS_PATH`, `COMMAND_STRING`, `MAX_INSTRS`, `RAMULATOR_CONFIG`.
- **[MOD] `simulator/ramulator/Controller.h`**: in the `Controller(const Config&, DRAM<T>*)`
  constructor, after `fake_ideal_DRAM(configs);`:
  ```cpp
  // OCCARB: configurable read/write queue depth (default 32 when absent)
  if (configs["queue_size"] != "") {
      readq.max = writeq.max = stoul(configs["queue_size"]);
  }
  if (channel->id == 0)
      printf("[RAMULATOR] Controller queue depth: readq=%u writeq=%u\n", readq.max, writeq.max);
  ```
  - Write-drain watermarks (`0.8*writeq.max` / `0.2*writeq.max`, `Controller.h:628-633`) scale
    automatically with the new depth.
  - `otherq` (refresh) is left at 32.
  - Applies to the generic `Controller<T>` (DDR3/DDR4/...). `HMC_Controller.h` is untouched.
- **[NEW] `simulator/occarb/rebuild.sh`**: `make clean && make libramulator.so` in `ramulator/`,
  then `scons`. A clean build is needed because Ramulator's Makefile has no header dependencies,
  so edits to `.h` files are not picked up by a plain `make`.
- **[NEW] `simulator/occarb/lib.sh`**: helpers `gen_cfg`, `run_dir`, `max_cycles`, `ncores`, `rstat`.
- **[NEW] `simulator/occarb/step2/run_check.sh`**: checks 2a–2e.

### Simulator behaviour changed?
- Configs without `queue_size`: no change, apart from 1 extra log line. Check 2a verifies this
  (Step 1 cycles must stay 26,637,522).
- Configs with `queue_size = N`: the read/write queues hold N entries.

### Undo
- Delete the 5 new files.
- Remove the 6 added lines from `Controller.h`.
- Run `bash occarb/rebuild.sh`.

### Script fixes after Step 2 run 1
- `occarb/lib.sh`: `max_cycles` now matches with grep. This system's awk is mawk, which doesn't
  support `[[:space:]]`. **Rule: never use POSIX character classes inside awk in these scripts.**
- `occarb/step2/run_check.sh`: 2a compares against the current-build reference 26,587,526
  and also requires Step 1 to be deterministic; 2d fails if cycles parse as 0.

### Methodology note: rebuilds change absolute cycles
- A rebuild of ZSim/Ramulator changed the Step 1 smoke result from 26,637,522 to 26,587,526
  cycles (−0.19%), with no functional change on that path. The likely cause is that a different
  tool binary changes the address-space layout, which changes physical address → bank mapping.
- Consequence: T_alone and T_overlap must always come from the same build. After each rebuild,
  re-run the baseline.

### Multi-core measurement noise (from Step 2)
- Two identical 4-core runs differ by about 0.12% in cycles, and slightly in DRAM request counts.
  Thread interleaving under ZSim depends on host scheduling.
- Rule: each multi-core configuration is run 3 times, and we report the mean (min–max).
- Single-core runs are bit-reproducible within a build.

### Build note
- In this (Claude) container, the newer GCC fails on `StatType.h` without `#include <cstdint>`.
  This is pre-existing and unrelated to OCCARB. The user's toolchain builds it as is.
  If a newer compiler is used later, add `#include <cstdint>` to `ramulator/StatType.h`.

---

## Step 3: DMA source tag

### What we did
Added a per-request source bit, so later steps can tell NIC DMA traffic from core traffic.
This is the "one source bit per queue entry" that the paper's cost analysis (§II-C) counts.

### Changes
- **[MOD] `simulator/ramulator/Request.h`**: inside `class Request`, right after `int coreid = -1;`:
  ```cpp
  // OCCARB: request source. false = core (cache hierarchy), true = NIC DMA (cache-bypassing)
  bool is_dma = false;
  ```
  - The in-class default means every existing constructor, and therefore every core request
    built in `src/ramulator_mem_ctrl.cpp` (`tick()`, `enqueue()`), is tagged as a core request.
  - The field is copied with the request into the controller queues.
  - This header is included by ZSim (`src/ramulator_mem_ctrl.cpp`), so **both** libraries must be
    rebuilt: `bash occarb/rebuild.sh`.
- **[NEW] `simulator/occarb/step3/test_request.cc`**: standalone unit test that needs no simulator.
- **[NEW] `simulator/occarb/step3/run_check.sh`**: checks 3a–3c.

### Simulator behaviour changed?
No. Nothing reads `is_dma` yet. Absolute cycles may shift by <0.5% from the layout effect.

### Undo
- Remove the 2 lines from `Request.h`.
- Delete `occarb/step3/`.
- Run `bash occarb/rebuild.sh`.

---

## Step 4: NIC model (standalone)

### What we did
Wrote the network-interface model from paper §II-A / §III as a self-contained class, and verified
it with unit tests before connecting it to the simulator.

### Changes
- **[NEW] `simulator/ramulator/NicModel.h`**: `ramulator::NicParams` + `ramulator::NicModel`.
  - It lives in `ramulator/` so that both the ZSim-side injector (Step 5) and the Ramulator
    scheduler (Steps 6–7) can include it.
  - Parameters (defaults): `buf_bytes` S (1 MiB), `link_gbps` (10), `cycle_hz` (1.2e9),
    `rho` (1.0), `period_cycles` (1e6), `pkt_bytes` (1500), `line_bytes` (64),
    `rx_enable`/`tx_enable` (true), `occ_bits` (2).
  - API:
    - `tick()`
    - `want_rx_write()` / `issue_rx_write()` → bytes / `complete_rx_write(bytes)`
    - `want_tx_read()` / `issue_tx_read()` / `complete_tx_read()`
    - `occupancy()`, `level()`, `active()`, `rx_loss_rate()`
    - public counters
- **[NEW] `simulator/occarb/step4/test_nic.cc`**, **[NEW] `simulator/occarb/step4/run_check.sh`**.

### Simulator behaviour changed?
No. Nothing includes `NicModel.h` yet (the check script verifies this), so no rebuild is needed.

### Undo
`rm ramulator/NicModel.h; rm -rf occarb/step4`
