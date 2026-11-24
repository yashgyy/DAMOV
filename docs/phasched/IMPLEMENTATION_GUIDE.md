# PhaSched — Implementation Guide

Steps are added here one at a time, as they are given in chat. Progress and results are recorded in
`PROGRESS_LOG.md`.

**Working rule for every step:** make the change → build → run the step's checks → commit → record in the log.

**Environment:** everything runs inside the Docker container `damov_phase` (`docker exec -it damov_phase /bin/bash`).
Repo path inside the container is **`/DAMOV`**. Always run ZSim from `/DAMOV/simulator`.
Files created inside the container are owned by root; to edit them from VS Code on the host either
`chown -R <host uid>:<host gid> /DAMOV/phasched` inside the container, or attach VS Code to the container.

---

## Step 0 — Baseline and test program

**Goal:** before changing the simulator, build a small test program, run it on DDR4 and save the output as the
reference ("golden") result. Every later step is compared against it.

No simulator code changes in this step.

### 0.1 Folders (from `DAMOV/` root)
```bash
mkdir -p phasched/include phasched/ubench phasched/configs/zsim phasched/configs/ramulator phasched/results/golden
```

### 0.2 Hint header — `phasched/include/phase_hint.h`
No-op for now; the CPU imitates the network card.
```c
#ifndef PHASE_HINT_H
#define PHASE_HINT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "zsim_hooks.h"

enum { FL_IDLE = 0, FL_INGRESS = 1, FL_TRAIN = 2, FL_EGRESS = 3 };

/* Step 0: no hints yet. CPU imitates the network card. */
static inline void fl_phase(int p) { (void)p; }
static inline void fl_dma_in(void* b, size_t n)  { memset(b, 1, n); }
static inline void fl_dma_out(void* b, size_t n) {
    volatile float s = 0; const float* f = (const float*)b;
    for (size_t i = 0; i < n / sizeof(float); i += 16) s += f[i];
}
#endif
```

### 0.3 Test program — `phasched/ubench/ubench.c`
Imitates one FL round: write the model (INGRESS), random reads + some writes (TRAIN), read the model (EGRESS).
Arguments: `model_MB work_MB rounds`.
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "phase_hint.h"
#define MB (1024UL*1024UL)

int main(int argc, char** argv) {
    size_t model = (argc > 1 ? atol(argv[1]) : 16) * MB;
    size_t work  = (argc > 2 ? atol(argv[2]) : 64) * MB;
    int rounds   =  argc > 3 ? atoi(argv[3]) : 1;
    float* m = aligned_alloc(4096, model);
    float* w = aligned_alloc(4096, work);
    memset(m, 0, model); memset(w, 1, work);
    size_t nm = model / sizeof(float), nw = work / sizeof(float);
    uint64_t x = 88172645463325252ULL; double acc = 0;

    zsim_roi_begin();
    for (int r = 0; r < rounds; r++) {
        fl_phase(FL_INGRESS); fl_dma_in(m, model);
        fl_phase(FL_TRAIN);
        for (size_t i = 0; i < nw; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            acc += w[x % nw];
            if ((i & 15) == 0) m[i % nm] += 1e-3f * (float)acc;
        }
        fl_phase(FL_EGRESS); fl_dma_out(m, model);
        fl_phase(FL_IDLE);
    }
    zsim_roi_end();
    printf("acc %f %f\n", acc, (double)m[0]);
    return 0;
}
```
Build:
```bash
gcc -O2 -I phasched/include -I simulator/misc/hooks phasched/ubench/ubench.c -o phasched/ubench/ubench
```

### 0.4 System config — `phasched/configs/zsim/base_ooo4_ddr4.cfg`
```bash
cp simulator/templates/template_host_ooo.cfg phasched/configs/zsim/base_ooo4_ddr4.cfg
```
Edit:

| Find | Change to |
|---|---|
| every `NUMBER_CORES` | `4` |
| `ramulatorConfig = "ramulator-configs/HMC-config.cfg";` | `ramulatorConfig = "ramulator-configs/DDR4-config.cfg";` |
| `maxTotalInstrs = 1000000000L;` | `maxTotalInstrs = 20000000000L;` |
| `stats = "STATS_PATH";` | `stats = "../phasched/results/ubench_base";` |
| `command = COMMAND_STRING` | `command = "/DAMOV/phasched/ubench/ubench 16 64 1";` |

`stats` must be **relative to `/DAMOV/simulator`** — Ramulator prepends the run directory to it (an absolute path
gives `/DAMOV/simulator//DAMOV/...` and the assertion `!stat_output.good()`). `command` must be an absolute
container path.

### 0.5 Run
Native (should just print `acc ...`):
```bash
./phasched/ubench/ubench 16 64 1
```
Simulator (run from `simulator/`, the Ramulator config path is relative to it):
```bash
cd /DAMOV/simulator && time ./build/opt/zsim ../phasched/configs/zsim/base_ooo4_ddr4.cfg
```

### ✅ Checks
1. Output shows `[RAMULATOR] Initialized Ramulator`, `[HOOKS] ROI begin`, `[HOOKS] ROI end`; finishes without panic.
2. `phasched/results/` contains `ubench_base.ramulator.stats` and `ubench_base.zsim.out` (names may vary slightly).
3. In `.ramulator.stats`: read requests, write requests, row hits, read latency are non-zero; standard is DDR4.
4. Determinism: run again with `stats` → `ubench_base2`, then
   ```bash
   diff phasched/results/ubench_base.ramulator.stats phasched/results/ubench_base2.ramulator.stats
   ```
   prints nothing (or only time / host-speed lines).
5. Copy the first run's files to `phasched/results/golden/step0.ramulator.stats` and `step0.zsim.out`.

### Report back
- Did all 5 checks pass? Any errors?
- From `ubench_base.ramulator.stats`: read requests, write requests, row hits, average read latency.
- Wall-clock time of the simulation.

### Step 0 result (golden, `phasched/results/golden/step0.*`)
`dram_cycles 1435415000`, `read_requests 16394048`, `write_requests 1310721`, `row_hits 1034622`,
`read_latency_avg 54.60` DRAM clk (45.50 ns). Two runs identical. Run time ≈ 5 min.

---

## Step 1 — Choose scheduler and knobs from the Ramulator config

**Goal:** select the scheduler (`FCFS`, `FRFCFS`, `FRFCFS_Cap`, default `FRFCFS_PriorHit`), queue size, write-drain
watermarks and row-hit cap from the Ramulator config file. **With no new keys, behaviour must be identical to
Step 0.**

File: `/DAMOV/simulator/ramulator/Controller.h` (only file changed).

### 1.1 New members
Find `bool write_mode = false;` (~line 180) and add directly below it:
```cpp
    double wq_high = 0.8;  // PhaSched: enter write-drain when writeq >= wq_high * max
    double wq_low  = 0.2;  // PhaSched: leave write-drain when writeq <= wq_low * max
```

### 1.2 Read the config keys
In the constructor find `fake_ideal_DRAM(configs);` (~line 230) and add directly below it:
```cpp
        // ---- PhaSched Step 1: configurable baseline knobs ----
        string sched = configs["scheduler"];
        if      (sched == "FCFS")       scheduler->type = Scheduler<T>::Type::FCFS;
        else if (sched == "FRFCFS")     scheduler->type = Scheduler<T>::Type::FRFCFS;
        else if (sched == "FRFCFS_Cap") scheduler->type = Scheduler<T>::Type::FRFCFS_Cap;
        else if (sched == "" || sched == "FRFCFS_PriorHit")
                                        scheduler->type = Scheduler<T>::Type::FRFCFS_PriorHit;
        else { printf("[PHASCHED] unknown scheduler '%s'\n", sched.c_str()); assert(false); }
        if (configs["row_hit_cap"] != "") scheduler->cap = stol(configs["row_hit_cap"]);
        if (configs["queue_size"]  != "") readq.max = writeq.max = stoi(configs["queue_size"]);
        if (configs["wq_high"]     != "") wq_high = stod(configs["wq_high"]);
        if (configs["wq_low"]      != "") wq_low  = stod(configs["wq_low"]);
        printf("[PHASCHED] ch%d scheduler=%s cap=%ld q=%u whi=%.2f wlo=%.2f\n",
               channel->id, sched == "" ? "FRFCFS_PriorHit(default)" : sched.c_str(),
               scheduler->cap, readq.max, wq_high, wq_low);
```

### 1.3 Use the watermarks
In `tick()`, section `/*** 3. Should we schedule writes? ***/` (~line 625):
- `int(0.8 * writeq.max)` → `int(wq_high * writeq.max)`
- `int(0.2 * writeq.max)` → `int(wq_low * writeq.max)`

### 1.4 Rebuild (header change → clean Ramulator build, then ZSim)
```bash
cd /DAMOV/simulator/ramulator && make clean && make libramulator.so
```
```bash
cd /DAMOV/simulator && sh scripts/compile.sh z
```

### 1.5 Configs for the baselines
```bash
cd /DAMOV/phasched/configs/ramulator
for s in FCFS FRFCFS FRFCFS_Cap; do
  cp /DAMOV/simulator/ramulator-configs/DDR4-config.cfg DDR4-$s.cfg
  printf "\n# PhaSched\n scheduler = %s\n queue_size = 32\n" $s >> DDR4-$s.cfg
done
echo " row_hit_cap = 16" >> DDR4-FRFCFS_Cap.cfg
cd /DAMOV/phasched/configs/zsim
for s in FCFS FRFCFS FRFCFS_Cap; do
  sed -e "s#ramulator-configs/DDR4-config.cfg#../phasched/configs/ramulator/DDR4-$s.cfg#" \
      -e "s#results/ubench_base\"#results/ubench_$s\"#" base_ooo4_ddr4.cfg > ubench_$s.cfg
done
```

### ✅ Checks
1. Build finishes without errors.
2. **Regression:** run `base_ooo4_ddr4.cfg` again (unchanged DDR4 config). Output shows
   `[PHASCHED] ch0 scheduler=FRFCFS_PriorHit(default) cap=16 q=32 whi=0.80 wlo=0.20` and
   ```bash
   diff /DAMOV/phasched/results/golden/step0.ramulator.stats /DAMOV/phasched/results/ubench_base.ramulator.stats && echo IDENTICAL
   ```
   prints `IDENTICAL`.
3. **Each baseline runs** (3 in parallel):
   ```bash
   cd /DAMOV/simulator
   for s in FCFS FRFCFS FRFCFS_Cap; do ./build/opt/zsim ../phasched/configs/zsim/ubench_$s.cfg > ../phasched/results/ubench_$s.log 2>&1 & done; wait
   grep PHASCHED ../phasched/results/ubench_*.log
   ```
   Each log shows its own scheduler name.
4. **Sensible differences:**
   ```bash
   cd /DAMOV/phasched/results
   for f in golden/step0 ubench_FCFS ubench_FRFCFS ubench_FRFCFS_Cap; do echo "== $f"; grep -E "ramulator\.(dram_cycles|row_hits|read_latency_avg) " $f.ramulator.stats; done
   ```
   Expect FCFS: fewest row hits, highest read latency. FRFCFS and FRFCFS_Cap close to each other and to the golden.
