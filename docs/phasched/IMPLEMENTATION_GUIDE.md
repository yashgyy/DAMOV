# PhaSched — Step-by-Step Implementation Guide (DAMOV-SIM)

Paper: *Round-Phase-Aware Memory Scheduling for Federated Learning Clients* (PhaSched).
This guide is written so you can implement each step yourself, in order, and **verify each step before moving on**.
Record what you did in `PROGRESS_LOG.md` after every step.

All paths are relative to the repo root (`DAMOV/`). Simulator code is in `simulator/`.

---

## 0. Ground rules (read once)

1. **One step at a time.** Build → run the step's test → check the pass criteria → commit → log.
2. **Ramulator is header-heavy.** `Controller.h`, `Scheduler.h`, `Memory.h`, `Request.h` are templates compiled into
   `libramulator.so`. The Makefile does **not** track header dependencies, so after editing any header always do a
   **clean** Ramulator rebuild, then rebuild ZSim:
   ```bash
   cd simulator/ramulator && make clean && make libramulator.so && cd ..
   ```
   ```bash
   cd simulator && sh scripts/compile.sh z
   ```
3. **Default must not change.** Until you set `scheduler = PhaSched` (or BLISS) in a config, the simulator must give
   **identical** results to the unmodified simulator. Every step re-checks this ("regression check").
4. **Commit after every passing step** (`git commit -m "phasched: step N ..."`), so you can bisect if something breaks.
5. Single-threaded micro-benchmark runs are deterministic in ZSim → regression checks can be exact. Multi-threaded
   runs may vary slightly (<1%).

### Key code locations (verified in this repo)

| What | File : line |
|---|---|
| Scheduler types (`FCFS, FRFCFS, FRFCFS_Cap, FRFCFS_PriorHit`), `cap = 16`, compare lambdas | `simulator/ramulator/Scheduler.h:27-31`, `:108-156` |
| Queue size `max = 32` | `simulator/ramulator/Controller.h:170` |
| Write-drain watermarks 0.8 / 0.2 (`write_mode`) | `simulator/ramulator/Controller.h:625-635` |
| Read completion / latency stat | `simulator/ramulator/Controller.h:597-610` |
| Request issue / row-hit stats / queue erase | `simulator/ramulator/Controller.h:657-715` |
| `issue_cmd` (every DRAM command passes here) | `simulator/ramulator/Controller.h:781` |
| Controller constructor (read config keys here) | `simulator/ramulator/Controller.h:210-235` |
| DRAM commands for DDR4 (`ACT, PRE, PREA, RD, WR, RDA, WRA, REF, ...`) | `simulator/ramulator/DDR4.h:32` |
| Request fields (`coreid`, `arrive`, `depart`) | `simulator/ramulator/Request.h` |
| `MemoryBase` virtual interface; `Memory<T>::ctrls` | `simulator/ramulator/Memory.h:29-42`, `:112` |
| ZSim↔Ramulator wrapper | `simulator/ramulator/RamulatorWrapper.{h,cc}` |
| ZSim Ramulator memory object (`tick`, `enqueue`, callbacks) | `simulator/src/ramulator_mem_ctrl.{h,cpp}` |
| Global pointer to Ramulator object | `zinfo->ramulator` (set in `simulator/src/init.cpp:381-383`) |
| Magic-op instrumentation (`xchg rcx,rcx`) | `simulator/src/zsim.cpp:620-623` |
| Magic-op handler `HandleMagicOp` | `simulator/src/zsim.cpp:1196-1272` |
| `procMask`, `lineBits` (virtual→line address) | `simulator/src/zsim.cpp:105-106` |
| App-side hooks header | `simulator/misc/hooks/zsim_hooks.h` |
| System template (OOO host) | `simulator/templates/template_host_ooo.cfg` |
| DDR4 config | `simulator/ramulator-configs/DDR4-config.cfg` |

### Folder layout we will add

```
DAMOV/phasched/
  include/phase_hint.h          # app-side hint + DMA helpers (Step 2 / Step 6)
  ubench/ubench.c               # micro-benchmark used for every verification
  configs/zsim/*.cfg            # ZSim system configs
  configs/ramulator/*.cfg       # one DDR4 config per scheduler
  scripts/run_matrix.sh         # runs all experiments
  scripts/analyze.py            # parses per-phase CSV -> tables/figures
  results/                      # outputs (git-ignored except summaries)
```

---

## Step 0 — Baseline and test harness (no simulator change) · ~2–3 h

**Goal:** a micro-benchmark + DDR4 system config, and a recorded "golden" output to compare every later step against.

### 0.1 System config
Copy `simulator/templates/template_host_ooo.cfg` to `phasched/configs/zsim/base_ooo4_ddr4.cfg` and edit:
- `NUMBER_CORES` → `4` (both in `cores` and the `caches = NUMBER_CORES` lines)
- `ramulatorConfig = "ramulator-configs/DDR4-config.cfg";` (template points to HMC — must change)
- `maxTotalInstrs` → large enough for one FL round (e.g. `20000000000L`); `maxOffloadInstrs` same
- `stats = "<abs path>/phasched/results/ubench_base";`
- `command = "<abs path>/phasched/ubench/ubench 16 64 1";`

### 0.2 Micro-benchmark `phasched/ubench/ubench.c`
It imitates one FL round: a sequential write of the "model" (INGRESS), random reads + sparse writes (TRAIN), and a
sequential read of the model (EGRESS). Args: `model_MB work_MB rounds`.

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
    memset(m, 0, model); memset(w, 1, work);          /* touch pages before ROI */
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

### 0.3 First version of `phasched/include/phase_hint.h`
Create it now with only the **no-hint** path, so Step 0/1 binaries never issue unknown magic ops (ZSim panics on
unknown ops). It is extended in Step 2 and Step 6.

```c
#ifndef PHASE_HINT_H
#define PHASE_HINT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "zsim_hooks.h"   /* from simulator/misc/hooks */

enum { FL_IDLE = 0, FL_INGRESS = 1, FL_TRAIN = 2, FL_EGRESS = 3 };

/* CPU proxy for the NIC, used until the DMA injector exists */
static inline void fl_cpu_in(void* b, size_t n)  { memset(b, 1, n); }
static inline void fl_cpu_out(void* b, size_t n) {
    volatile float s = 0; const float* f = (const float*)b;
    for (size_t i = 0; i < n / sizeof(float); i += 16) s += f[i];
}
static inline void fl_phase(int p) { (void)p; }
#define fl_dma_in  fl_cpu_in
#define fl_dma_out fl_cpu_out
#endif
```

Build:
```bash
gcc -O2 -I phasched/include -I simulator/misc/hooks phasched/ubench/ubench.c -o phasched/ubench/ubench
```

### 0.4 Run and save golden
```bash
cd simulator && ./build/opt/zsim ../phasched/configs/zsim/base_ooo4_ddr4.cfg
```
Save `results/ubench_base.zsim.out` and `results/ubench_base.ramulator.stats` as
`phasched/results/golden/step0.*`.

### ✅ Pass criteria
- Simulation finishes; stdout contains `[RAMULATOR] Initialized Ramulator` and `[HOOKS] ROI begin/end`.
- `ramulator.stats` shows DDR4 (not HMC) and non-zero read/write requests, row hits, read latency.
- Native run `./phasched/ubench/ubench 16 64 1` also works (hooks are NOPs natively).
- Running the same config twice gives identical `ramulator.stats` (determinism check:
  `diff <(grep -v -i time run1) <(grep -v -i time run2)`).

---

## Step 1 — Config knobs: scheduler type, queue size, watermarks, cap · ~2–3 h

**Goal:** choose the scheduler and knobs from the Ramulator config file. **No behaviour change by default.**

### Changes
`simulator/ramulator/Controller.h`
1. Add members next to `write_mode`:
   ```cpp
   double wq_high = 0.8, wq_low = 0.2;   // write-drain watermarks (fraction of writeq.max)
   ```
2. In the constructor, after `fake_ideal_DRAM(configs);` add:
   ```cpp
   // ---- PhaSched: configurable baseline knobs ----
   string sched = configs["scheduler"];
   if      (sched == "FCFS")            scheduler->type = Scheduler<T>::Type::FCFS;
   else if (sched == "FRFCFS")          scheduler->type = Scheduler<T>::Type::FRFCFS;
   else if (sched == "FRFCFS_Cap")      scheduler->type = Scheduler<T>::Type::FRFCFS_Cap;
   else if (sched == "FRFCFS_PriorHit" || sched == "") scheduler->type = Scheduler<T>::Type::FRFCFS_PriorHit;
   else { printf("[PHASCHED] unknown scheduler %s\n", sched.c_str()); assert(false); }
   if (configs["row_hit_cap"] != "") scheduler->cap = stol(configs["row_hit_cap"]);
   if (configs["queue_size"]  != "") readq.max = writeq.max = stoi(configs["queue_size"]);
   if (configs["wq_high"]     != "") wq_high = stod(configs["wq_high"]);
   if (configs["wq_low"]      != "") wq_low  = stod(configs["wq_low"]);
   printf("[PHASCHED] ch%d scheduler=%s cap=%ld q=%u whi=%.2f wlo=%.2f\n", channel->id,
          sched == "" ? "FRFCFS_PriorHit(default)" : sched.c_str(), scheduler->cap, readq.max, wq_high, wq_low);
   ```
3. In `tick()` replace the literals `0.8` and `0.2` with `wq_high` and `wq_low`.

Create `phasched/configs/ramulator/DDR4-<SCHED>.cfg` = copy of `DDR4-config.cfg` plus one line
`scheduler = FCFS` / `FRFCFS` / `FRFCFS_Cap` (later `BLISS`, `PhaSched`). Also add `queue_size = 32` explicitly
(paper Table II value) so it is visible in every config.

> **Baseline definitions for the paper:** "FR-FCFS" = `FRFCFS`; "FR-FCFS-Cap" = `FRFCFS_Cap` with cap 16.
> DAMOV's default `FRFCFS_PriorHit` is not one of the paper's baselines; keep it only for the regression check.

### ✅ Verify
1. Clean rebuild (Section 0). Stdout shows one `[PHASCHED] ch0 ...` line per channel.
2. **Regression:** original `DDR4-config.cfg` (no `scheduler` key) → `ramulator.stats` identical to `golden/step0`.
3. **Sensitivity:** `scheduler = FCFS` → row-hit count goes down, average read latency goes up vs FRFCFS.
4. `queue_size = 64` → the printed line shows `q=64`, run completes.

---

## Step 2 — Phase interface: magic op → phase register · ~3–4 h

**Goal:** the app announces the phase; Ramulator's controllers hold it. **Still no behaviour change.**

### 2.1 App side — extend `phase_hint.h`
Add (keep the CPU-proxy path for builds with `-DPHASCHED_NO_DMA`, used until Step 6):

```c
#define ZSIM_MAGIC_OP_PHASE      (1040)
#define ZSIM_MAGIC_OP_DMA_WRITE  (1041)   /* NIC -> DRAM (ingress) : rdx=addr, rsi=bytes */
#define ZSIM_MAGIC_OP_DMA_READ   (1042)   /* DRAM -> NIC (egress)  : rdx=addr, rsi=bytes */
#define ZSIM_MAGIC_OP_DMA_POLL   (1043)   /* returns 1 in rdx while DMA busy */

static inline void zsim_op2(uint64_t op, uint64_t a, uint64_t b) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "d"(a), "S"(b));
    COMPILER_BARRIER();
}
static inline uint64_t zsim_op_ret(uint64_t op) {
    uint64_t r = 0;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : "+d"(r) : "c"(op));
    COMPILER_BARRIER();
    return r;
}
#ifdef PHASCHED_NO_HINTS
static inline void fl_phase(int p) { (void)p; }
#else
static inline void fl_phase(int p) { zsim_op2(ZSIM_MAGIC_OP_PHASE, (uint64_t)p, 0); }
#endif
```
Natively these are NOPs (and `DMA_POLL` returns 0), so workloads still run outside ZSim.

### 2.2 ZSim side — `simulator/src/zsim.cpp`
1. Instrumentation (line ~622): pass RDX by reference and RSI by value:
   ```cpp
   INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) HandleMagicOp, IARG_THREAD_ID,
                  IARG_REG_VALUE, REG_ECX, IARG_REG_REFERENCE, REG_RDX, IARG_REG_VALUE, REG_RSI, IARG_END);
   ```
2. Signature: `VOID HandleMagicOp(THREADID tid, ADDRINT op, PIN_REGISTER* rdx, ADDRINT rsi)`.
3. Add the defines (same numbers as app side) and the case:
   ```cpp
   case ZSIM_MAGIC_OP_PHASE:
       if (zinfo->ramulator) zinfo->ramulator->requestPhase((int)rdx->qword[0]);
       return;
   ```

### 2.3 Ramulator object — `simulator/src/ramulator_mem_ctrl.{h,cpp}`
**Rule:** `HandleMagicOp` runs on the application thread (bound phase); `tick()` runs in the weave phase.
The magic op only stores an integer; the real work is done in `tick()`.
```cpp
// .h  (public)
void requestPhase(int p) { pendingPhase = p; }
// .h  (private)
volatile int pendingPhase = -1;
int curPhase = 0;
uint64_t phaseStartCycle = 0;
std::ofstream phaseLog;            // opened in constructor: application_name + ".phases.csv"
// .cpp, at the top of Ramulator::tick()
if (pendingPhase >= 0) {
    int p = pendingPhase; pendingPhase = -1;
    phaseLog << "phase_change," << curPhase << "," << phaseStartCycle << "," << curCycle << "\n";
    curPhase = p; phaseStartCycle = curCycle;
    wrapper->set_phase(p);
}
```
(`curCycle` counts CPU cycles — the Ramulator tick event runs every core cycle.)

### 2.4 Plumb into Ramulator
- `RamulatorWrapper.h/.cc`: `void set_phase(int p) { mem->set_phase(p); }`
- `Memory.h`, class `MemoryBase`: `virtual void set_phase(int p) {}` (default body — keeps HMC etc. compiling).
- `Memory<T>`: `void set_phase(int p) override { for (auto c : ctrls) c->phase = p; }`
- `Controller.h`: `int phase = 0;  // 0 IDLE, 1 INGRESS, 2 TRAIN, 3 EGRESS`

### ✅ Verify
1. Build ubench with `-DPHASCHED_NO_DMA` (hints on, DMA proxied by CPU).
2. `*.phases.csv` has 4 `phase_change` lines per round in order 0→1→2→3→0; cycle stamps strictly increase.
3. Phase durations look sensible: INGRESS ≈ time to memset 16 MB, TRAIN the longest.
4. **Regression:** Ramulator stats identical to Step 1 with the same scheduler (phase does nothing yet).
5. Build ubench with `-DPHASCHED_NO_HINTS`: no `phase_change` lines, still identical.

---

## Step 3 — Per-phase metrics · ~3–4 h

**Goal:** every number in the paper comes from one CSV: per phase × channel → duration, reads, read latency
(avg, P50, P99), writes, bus turnarounds, ACT/PRE/RD/WR/REF counts, row hits.

### Changes — `Controller.h`
1. Add `bool is_dma = false;` to `Request` (`Request.h`) — set later by the DMA injector.
2. Counters in `Controller`:
   ```cpp
   struct PhaseCnt { long reads=0, rd_lat_sum=0, dma_reads=0, writes=0, turnarounds=0,
                     act=0, pre=0, rd=0, wr=0, ref=0, row_hits=0; } pc;
   vector<long> lat_hist = vector<long>(4097, 0);   // core read latency in DRAM clocks, last bin = overflow
   ```
3. Increment:
   - Read completion (step "1. Serve completed reads", where `read_latency_sum` is updated): if `!req.is_dma`
     → `pc.reads++; pc.rd_lat_sum += lat; lat_hist[min(lat,4096L)]++;` else `pc.dma_reads++`.
   - Write issued (where `write_transaction_bytes` is updated): `pc.writes++`.
   - Row hit (where `++(*row_hits)`): `pc.row_hits++`.
   - `write_mode` toggles (both directions): `pc.turnarounds++`.
   - In `issue_cmd`: `ACT→act`, `PRE/PREA→pre`, `RD/RDA→rd` (RDA also `pre++`), `WR/WRA→wr` (WRA also `pre++`),
     `REF→ref`.
4. `void dump_phase(ostream& o, int ph, uint64_t c0, uint64_t c1)` writes one CSV row with P50/P99 computed from
   `lat_hist`, then resets `pc` and `lat_hist`.
5. Plumb `phase_snapshot(ostream&, int ph, uint64_t c0, uint64_t c1)` through `MemoryBase` (default no-op) →
   `Memory<T>` (loop over `ctrls`) → `RamulatorWrapper`.
6. In `Ramulator::tick()` at a phase change, call `wrapper->phase_snapshot(phaseLog, curPhase, phaseStartCycle,
   curCycle)` **before** switching phase; also once in `Ramulator::finish()` for the last phase.

CSV header (write once when opening the file):
`phase,start_cycle,end_cycle,ch,reads,rd_lat_sum,rd_lat_p50,rd_lat_p99,dma_reads,writes,turnarounds,act,pre,rd,wr,ref,row_hits`
(latencies in DRAM clocks; DDR4-2400 tCK = 0.833 ns).

### ✅ Verify
1. Σ reads + Σ dma_reads over all rows = total reads in `ramulator.stats` (±in-flight at ROI end); same for writes.
2. Σ (end−start) over rows = total simulated cycles of the ROI.
3. avg latency = `rd_lat_sum/reads` from the CSV matches `ramulator.stats` average when only one phase exists
   (`-DPHASCHED_NO_HINTS`).
4. INGRESS rows (CPU proxy) are write-heavy; EGRESS rows are read-only; turnarounds > 0 in TRAIN.
5. **Regression:** `ramulator.stats` still identical to Step 1 (counters are read-only additions).

---

## Step 4 — PhaSched presets + starvation guard · ~4–6 h

**Goal:** the actual mechanism (Paper Table I).

### Design decision (state it in the paper)
PhaSched is built on **FR-FCFS-Cap**: IDLE = FR-FCFS-Cap with Whi/Wlo = 0.8/0.2 and C = 16. So "IDLE behaves exactly
as the baseline" is true and testable.

| Phase | Enter write mode | Leave write mode | Row-hit cap C |
|---|---|---|---|
| IDLE | `wq ≥ Whi` or `rq == 0` (baseline) | `wq ≤ Wlo` and `rq > 0` (baseline) | 16 |
| INGRESS (eager) | `wq > 0` | `wq == 0` | ∞ (`LONG_MAX`) |
| TRAIN (lazy) | `wq ≥ Whi` or (`rq == 0` and `wq > 0`) | as soon as `rq > 0` and `wq < Whi` | 16 |
| EGRESS | only `wq ≥ Whi` | `wq ≤ Wlo` | raised, default 64 |

Starvation guard (all phases, PhaSched only): if the **oldest** request in the queue being served waited
> `Tmax` DRAM clocks, serve it next; if the oldest request in the **other** queue waited > `Tmax`, switch mode.

### Changes
1. `Scheduler.h`: add `BLISS, PhaSched` to the enum **before `MAX`** and add two lambdas to `compare[]` in the same
   order (put a temporary copy of FRFCFS for BLISS until Step 5). PhaSched lambda = FRFCFS_Cap lambda but using
   `this->ctrl->phase_cap()` instead of `this->cap`.
2. `Scheduler::get_head`: for `PhaSched`, first scan for the oldest request with `ctrl->clk - arrive > ctrl->tmax`;
   if found return it.
3. `Controller.h`:
   - Members: `long tmax = 2000; long egress_cap = 64;` read from config keys `phasched_tmax`, `phasched_egress_cap`.
   - `long phase_cap()` returns per-phase cap from the table.
   - Move the write-mode block of `tick()` into `void update_write_mode()` implementing the table when
     `scheduler->type == PhaSched`, baseline code otherwise. Count `pc.turnarounds` there.
   - Constructor: accept `scheduler = PhaSched` (and `BLISS`).
4. `phasched/configs/ramulator/DDR4-PhaSched.cfg`: `scheduler = PhaSched`, `row_hit_cap = 16`,
   `phasched_tmax = 2000`, `phasched_egress_cap = 64`.

### ✅ Verify
1. **Regression A:** all old schedulers identical to Step 3.
2. **Regression B (key test):** PhaSched with `-DPHASCHED_NO_HINTS` (always IDLE) == FRFCFS_Cap, stats identical.
3. **Effect** (ubench, hints on, CPU-proxy DMA), PhaSched vs FRFCFS_Cap per phase:
   - INGRESS: turnarounds drop sharply, row-hit rate up.
   - TRAIN: core read avg/P99 latency down (writes postponed).
   - EGRESS: row-hit rate up.
4. Starvation: set `phasched_tmax = 50` → P99 read latency drops, overall time changes; with a huge Tmax
   (1e9) there is no deadlock (the run completes).
5. Add a temporary `printf` of mode switches for ~100 cycles around a phase change and eyeball it, then remove it.

---

## Step 5 — BLISS baseline · ~3–4 h

**Goal:** Subramanian et al., ICCD 2014. A source that gets ≥ `bliss_threshold` (4) requests served **in a row**
is blacklisted; the blacklist is cleared every `bliss_clear` (10 000) DRAM cycles. Priority:
non-blacklisted > row-hit/ready > oldest.

### Changes
1. `Scheduler.h` members: `int nsrc; vector<bool> black; int last_src = -1, streak = 0;
   long last_clear = 0, threshold = 4, clear_int = 10000; long blacklistings = 0;`
   Source id: `req.is_dma ? nsrc-1 : req.coreid` with `nsrc = num_cores + 1` (DMA = own source).
2. `void on_served(const Request& r)` (called by `Controller` right before `queue->q.erase(req)`): update streak,
   blacklist if `streak >= threshold`, clear when `ctrl->clk - last_clear >= clear_int`.
3. BLISS lambda in `compare[]`: blacklisted status first, then `is_ready`, then `arrive`.
4. `Controller` ctor sets `scheduler->nsrc = configs.get_core_num() + 1` and reads `bliss_threshold`,
   `bliss_clear`. Print `blacklistings` at the end (`finish()`).

### ✅ Verify
1. Single-thread ubench: BLISS ≈ FR-FCFS (only one source → everyone equal); `blacklistings` > 0.
2. Two-thread test (one thread streams, one does random reads; or ubench with DMA in Step 6): the streaming source
   is blacklisted most often; random-reader latency lower than with FR-FCFS.
3. Regressions A and B from Step 4 still hold.

---

## Step 6 — DMA injector (the NIC) · ~5–7 h

**Goal:** INGRESS/EGRESS traffic is generated by a modeled NIC at the link rate, bypassing caches, and the app
waits until the transfer completes.

### 6.1 App side (`phase_hint.h`, default build — no `PHASCHED_NO_DMA`)
```c
static inline void fl_dma_wait(void) {
    while (zsim_op_ret(ZSIM_MAGIC_OP_DMA_POLL)) __builtin_ia32_pause();
}
static inline void fl_dma_in (void* b, size_t n) { zsim_op2(ZSIM_MAGIC_OP_DMA_WRITE, (uint64_t)b, n); fl_dma_wait(); }
static inline void fl_dma_out(void* b, size_t n) { zsim_op2(ZSIM_MAGIC_OP_DMA_READ,  (uint64_t)b, n); fl_dma_wait(); }
```
Keep the `#ifdef PHASCHED_NO_DMA` branch mapping to the CPU proxy.

### 6.2 ZSim side (`zsim.cpp`, `HandleMagicOp`)
```cpp
case ZSIM_MAGIC_OP_DMA_WRITE:
case ZSIM_MAGIC_OP_DMA_READ: {
    Address line = procMask | ((Address)rdx->qword[0] >> lineBits);   // same mapping ZSim uses for caches
    if (zinfo->ramulator) zinfo->ramulator->requestDma(line << lineBits, rsi, op == ZSIM_MAGIC_OP_DMA_WRITE);
    return;
}
case ZSIM_MAGIC_OP_DMA_POLL:
    rdx->qword[0] = (zinfo->ramulator && zinfo->ramulator->dmaBusy()) ? 1 : 0;
    return;
```

### 6.3 Ramulator object (`ramulator_mem_ctrl.{h,cpp}`)
- State: `volatile bool dmaPending; uint64_t dmaReqAddr, dmaReqBytes; bool dmaReqWrite;`
  `bool dmaActive; uint64_t dmaNext, dmaEnd; long dmaOutstanding; double dmaNextIssue, dmaInterval;`
- `requestDma(...)` only stores the request and sets `dmaPending = true` (bound phase).
- `dmaBusy()` returns `dmaPending || dmaActive`.
- In `tick()`: if `dmaPending` → start (`dmaNext = addr`, `dmaEnd = addr + bytes` rounded to 64 B,
  `dmaNextIssue = curCycle`). While active and `curCycle >= dmaNextIssue` and `dmaNext < dmaEnd`:
  build `ramulator::Request req(dmaNext, WRITE/READ, dma_cb_func, 0); req.is_dma = true;` and if
  `wrapper->send(req)` succeeds → `dmaNext += 64; dmaOutstanding++; dmaNextIssue += dmaInterval;`
  (on failure retry next cycle — back-pressure from a full queue).
- `dma_cb_func` → `DRAM_dma_return_cb`: `dmaOutstanding--`; when `dmaNext >= dmaEnd && dmaOutstanding == 0`
  → `dmaActive = false`. **Do not** use `DRAM_read_return_cb` — it asserts the address is in `inflightRequests`.
- `dmaInterval = cpuFreqHz * 64 / (linkGbps * 1e9 / 8)` → 10 GbE @ 2.4 GHz = **122.88 cycles/line**.
- Link rate: in `init.cpp` Ramulator branch, read `config.get<double>("sys.mem.dmaLinkGbps", 10.0)` and call
  `setDmaLink(gbps)` on the new object. Add `dmaLinkGbps = 10.0;` to `sys.mem` in the ZSim config.
- `coreid = 0` keeps Ramulator's per-core stat vectors in range; `is_dma` distinguishes it.

### ✅ Verify (ubench 16 MB model, default build)
1. INGRESS duration ≈ 16 MiB / 1.25 GB/s = 13.42 ms = **32.2 M cycles** (±2 %). EGRESS same.
2. INGRESS row: `writes` ≈ 262 144 (16 MiB / 64 B), `dma_reads` = 0. EGRESS row: `dma_reads` ≈ 262 144.
3. Set `dmaLinkGbps = 1000` → INGRESS becomes DRAM-bound (a few ms); check it is ≥ 16 MiB / 19.2 GB/s
   (DDR4-2400 x64 peak) ≈ **0.87 ms ≈ 2.1 M cycles**.
4. Model 32 MB → INGRESS/EGRESS durations double.
5. Native run of ubench still works (poll returns 0).
6. Now PhaSched vs FRFCFS_Cap on ubench: INGRESS/EGRESS turnarounds near 0 with PhaSched.

> Known limitation (write in paper): DMA writes bypass but do not invalidate cached copies of the buffer.
> Acceptable because the client copies the received model out of the receive buffer.

---

## Step 7 — Integrate the FL workloads · ~3–4 h

For each client (logistic regression, naive Bayes, DNN, LSTM, GRU):
1. `#include "phase_hint.h"`; add `-I phasched/include -I simulator/misc/hooks` to its build.
2. Round loop:
   ```c
   zsim_roi_begin();                                   // once, before the first measured round
   for (round ...) {
       fl_phase(FL_INGRESS); recv_model(buf, bytes); fl_dma_in(buf, bytes);
       fl_phase(FL_TRAIN);   local_train(...);          // E epochs or K minibatches
       fl_phase(FL_EGRESS);  fl_dma_out(buf, bytes); send_update(buf, bytes);
       fl_phase(FL_IDLE);
   }
   zsim_roi_end();
   ```
3. Server: either run natively alongside (localhost socket) or — recommended for determinism — compile the client
   with a **stub server** (`-DFL_STUB_SERVER`: `recv_model` = load from file/memcpy, `send_update` = no-op).
4. Make model sizes configurable on the command line; targets: LogReg 1M×10 (~40 MB), NB 1M×10 (~40 MB),
   DNN 784-2048-2048-10 (~23 MB), LSTM 2×1024 in 512 (~30 MB), GRU 2×1024 in 512 (~23 MB).
5. Threads: training threads = 4 (= cores). If a client is single-threaded, state it.

### ✅ Verify
- Native run: same accuracy/loss with and without hints (hints are NOPs).
- ZSim run of 1 round: `phases.csv` has INGRESS/TRAIN/EGRESS rows; INGRESS ≈ bytes / 1.25 GB/s.
- Record simulation speed (host seconds per simulated M cycles) → choose K minibatches so one round is
  ≤ ~3–4 h of host time. Log the chosen K per workload.

---

## Step 8 — Experiment matrix · ~2 h setup + run time

- Schedulers: `FCFS, FRFCFS, FRFCFS_Cap, BLISS, PhaSched` → 5 Ramulator configs.
- Workloads: 5 → 25 ZSim configs (`stats` path = `results/<wl>/<sched>`).
- Extra: model-size sweep (LogReg 1 / 10 / 40 MB) × (FRFCFS, PhaSched); Tmax / egress-cap sweep for PhaSched on 1–2
  workloads.
- `scripts/run_matrix.sh`: one line per run, executed with `xargs -P 4` (container/host has 4 cores); each run logs
  stdout to `results/<wl>/<sched>.log`.

### ✅ Verify
- All runs finish; each has `*.phases.csv`, `*.ramulator.stats`, `*.zsim.out`.
- The IDLE-equivalence and determinism checks (Step 4 regression B) pass on one real workload.

---

## Step 9 — Analysis, energy, figures, paper · ~4–6 h

`scripts/analyze.py` reads every `phases.csv` and produces:
- Round time per workload/scheduler = Σ phase durations of one round; normalised to FRFCFS → **Fig. 2** (+ geomean).
- Per-phase stacked bars (INGRESS/TRAIN/EGRESS).
- TRAIN core read latency avg and P99; turnarounds per phase; row-hit rate.
- DRAM energy from command counts (IDD values from the Micron 4Gb x8 DDR4-2400 datasheet, VDD = 1.2 V,
  8 chips per rank):
  - `E_ACTPRE = VDD·(IDD0·tRC − IDD3N·tRAS − IDD2N·(tRC−tRAS))`
  - `E_RD = VDD·(IDD4R − IDD3N)·tBURST`, `E_WR = VDD·(IDD4W − IDD3N)·tBURST` (tBURST = 4 tCK)
  - `E_REF = VDD·(IDD5B − IDD3N)·tRFC`
  - `E_BG ≈ VDD·IDD3N·T_phase`
  - total = chips × (act·E_ACTPRE + rd·E_RD + wr·E_WR + ref·E_REF + E_BG)

### Paper placeholder mapping
| Placeholder | Computed as |
|---|---|
| [N] | number of workloads (5) |
| [X%] / [Xmax%] | 1 − geomean / min of normalised round time (PhaSched vs FRFCFS) |
| [A%] / [B%] | reduction of INGRESS / EGRESS duration |
| [T%] | reduction of turnarounds in INGRESS+EGRESS |
| [Y%] | reduction of TRAIN avg core read latency |
| [Z%] | BLISS round-time gain vs FRFCFS |
| [E%] | DRAM energy change PhaSched vs FRFCFS |

---

## Quick checklist after **every** change

1. Clean-rebuild Ramulator, rebuild ZSim — no warnings about the files you touched.
2. ubench (`-DPHASCHED_NO_HINTS`) with original `DDR4-config.cfg` → `ramulator.stats` equal to golden.
3. The step's own feature test (above).
4. Commit, then add an entry to `PROGRESS_LOG.md`.
