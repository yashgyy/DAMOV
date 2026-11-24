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
