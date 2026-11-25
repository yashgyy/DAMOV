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

### 1.6 Bug fix: `RowTable::get_hits` (needed for FRFCFS_Cap and later PhaSched)
Symptom: FRFCFS_Cap crashes with
`Scheduler.h:240 RowTable<T>::update ... Assertion 'match->second.row == row' failed`.
Cause: `get_hits()` uses `table[rowgroup]`, which **inserts** a fake entry (row 0) for a closed bank; the next ACT
uses `table.insert(...)`, which does not overwrite an existing key, so the table keeps row 0 and the next RD/WR asserts.
Only `FRFCFS_Cap` calls `get_hits` (Scheduler.h:132-133), so other schedulers are unaffected.

In `/DAMOV/simulator/ramulator/Scheduler.h`, `int get_hits(vector<int>& addr_vec)` — replace
```cpp
        auto itr = table.find(rowgroup);
        auto entry = table[rowgroup];
        if (itr == table.end() || entry.row != row)
            return 0;

        return entry.hits;
```
with
```cpp
        auto itr = table.find(rowgroup);          // PhaSched fix: never insert here
        if (itr == table.end() || itr->second.row != row)
            return 0;

        return itr->second.hits;
```
Rebuild as in 1.4.

### Regression rule (updated after Step 1)
ZSim runs are **not bit-identical** run-to-run: three runs of the same config gave dram_cycles 1,435,415,000 /
1,435,446,000 / 1,435,563,000 (spread 0.01 %). A regression check therefore passes when `dram_cycles`,
`read_requests` and `read_latency_avg` are within **±0.1 %** of the reference. Use the compare script below instead of
`diff`.

`/DAMOV/phasched/scripts/cmp.sh`:
```bash
#!/bin/bash
# usage: cmp.sh ref.ramulator.stats new.ramulator.stats
for k in dram_cycles read_requests write_requests row_hits read_latency_avg; do
  a=$(grep -E "ramulator\.$k " "$1" | awk '{print $2}')
  b=$(grep -E "ramulator\.$k " "$2" | awk '{print $2}')
  awk -v k=$k -v a=$a -v b=$b 'BEGIN{d=(b-a)/a*100; s=(d<0?-d:d)<=0.1?"OK":"DIFF";
       printf "%-18s %16s %16s %+8.3f%%  %s\n",k,a,b,d,s}'
done
```

### Step 1 result
FRFCFS_Cap runs after the fix; all schedulers within ±0.1 % of golden on ubench (random-access test, so the cap and
FR-FCFS rarely matter). FCFS +1.0 % DRAM cycles, +4.4 % read latency. ✅

---

## Step 2 — Phase interface (app → ZSim → Ramulator)

**Goal:** the program announces the round phase with a magic instruction; ZSim passes it to Ramulator; every
memory controller stores it in `int phase`. Phase changes are logged with their cycle. **Still no behaviour
change** (nothing reads `phase` yet).

Rule: `HandleMagicOp` runs on the application thread, `Ramulator::tick()` on the simulation (weave) thread.
The magic op only stores a number; `tick()` applies it.

Before editing, back up each file: `cp <file> <file>.step1`.

### 2.1 App side — `/DAMOV/phasched/include/phase_hint.h`
Replace the line `static inline void fl_phase(int p) { (void)p; }` with:
```c
#define ZSIM_MAGIC_OP_PHASE (1040)   /* rdx = phase */

static inline void zsim_op2(uint64_t op, uint64_t a, uint64_t b) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op), "d"(a), "S"(b));
    COMPILER_BARRIER();
}
#ifdef PHASCHED_NO_HINTS
static inline void fl_phase(int p) { (void)p; }
#else
static inline void fl_phase(int p) { zsim_op2(ZSIM_MAGIC_OP_PHASE, (uint64_t)p, 0); }
#endif
```
(`fl_dma_in/out` stay as CPU proxies until Step 6.) Rebuild ubench (Step 0.3 command).

### 2.2 ZSim — `/DAMOV/simulator/src/zsim.cpp`
1. Line ~149 (declaration): `VOID HandleMagicOp(THREADID tid, ADDRINT op);` →
   `VOID HandleMagicOp(THREADID tid, ADDRINT op, PIN_REGISTER* rdx, ADDRINT rsi);`
2. Line ~622 (instrumentation):
   ```cpp
   INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) HandleMagicOp, IARG_THREAD_ID,
                  IARG_REG_VALUE, REG_ECX, IARG_REG_REFERENCE, LEVEL_BASE::REG_RDX,
                  IARG_REG_VALUE, LEVEL_BASE::REG_RSI, IARG_END);
   ```
   (Must be `LEVEL_BASE::` — plain `REG_RDX`/`REG_RSI` clash with `<sys/ucontext.h>`: "reference to 'REG_RDX' is ambiguous".)
3. Next to `#define ZSIM_MAGIC_OP_FUNCTION_END (1032)` (~line 1194): `#define ZSIM_MAGIC_OP_PHASE (1040)`
4. Definition line ~1196: same new signature as in 1.
5. In the `switch`, before `// HACK: Ubik magic ops`:
   ```cpp
        case ZSIM_MAGIC_OP_PHASE:
            if (zinfo->ramulator_memory && zinfo->ramulator)
                zinfo->ramulator->requestPhase((int)rdx->qword[0]);
            return;
   ```

### 2.3 ZSim Ramulator object — `/DAMOV/simulator/src/ramulator_mem_ctrl.h`
Add `#include <fstream>` at the top. In `class Ramulator`:
- `public:` add `void requestPhase(int p) { pendingPhase = p; }`
- `private:` add
  ```cpp
    volatile int pendingPhase = -1;   // set by magic op (app thread)
    int curPhase = 0;                 // 0 IDLE, 1 INGRESS, 2 TRAIN, 3 EGRESS
    uint64_t phaseStartCycle = 0;
    std::ofstream* phaseLog = nullptr;
  ```

### 2.4 `/DAMOV/simulator/src/ramulator_mem_ctrl.cpp`
- End of the constructor (after `name = _name;`):
  ```cpp
  phaseLog = new std::ofstream(application_name + ".phases.csv");
  *phaseLog << "event,from_phase,to_phase,start_cycle,end_cycle\n";
  ```
- First lines of `Ramulator::tick(uint64_t cycle)`:
  ```cpp
  if (pendingPhase >= 0) {
    int p = pendingPhase; pendingPhase = -1;
    *phaseLog << "phase," << curPhase << "," << p << "," << phaseStartCycle << "," << curCycle << "\n";
    curPhase = p; phaseStartCycle = curCycle;
    wrapper->set_phase(p);
  }
  ```
- In `Ramulator::finish()`, first lines:
  ```cpp
  *phaseLog << "end," << curPhase << ",-1," << phaseStartCycle << "," << curCycle << "\n";
  phaseLog->flush();
  ```
(`curCycle` counts CPU cycles: tick runs every core cycle.)

### 2.5 Plumbing into Ramulator
- `/DAMOV/simulator/ramulator/RamulatorWrapper.h`, in the class (public): `void set_phase(int p);`
- `/DAMOV/simulator/ramulator/RamulatorWrapper.cc`: `void RamulatorWrapper::set_phase(int p) { mem->set_phase(p); }`
- `/DAMOV/simulator/ramulator/Memory.h`, class `MemoryBase` (~line 41): `virtual void set_phase(int p) {}`
- `Memory.h`, class `Memory` (public, e.g. just before `bool send(Request req)` ~line 424):
  `void set_phase(int p) { for (auto ctrl : ctrls) ctrl->phase = p; }`
- `/DAMOV/simulator/ramulator/Controller.h`, next to `write_mode`: `int phase = 0;  // PhaSched round phase`

### 2.6 Rebuild
```bash
cd /DAMOV/simulator/ramulator && make clean && make libramulator.so
```
```bash
cd /DAMOV/simulator && sh scripts/compile.sh z
```
```bash
cd /DAMOV && gcc -O2 -I phasched/include -I simulator/misc/hooks phasched/ubench/ubench.c -o phasched/ubench/ubench
```

### ✅ Checks
1. Builds pass; `./phasched/ubench/ubench 16 64 1` still runs natively (hint is a no-op outside ZSim).
2. Run base config (output to log, wait for it). Then
   `cat /DAMOV/phasched/results/ubench_base.phases.csv` shows, in order:
   `phase,0,1` → `phase,1,2` → `phase,2,3` → `phase,3,0` → `end,0,-1`, with increasing cycles.
3. Plausible durations (end−start): TRAIN ≫ INGRESS, EGRESS (TRAIN ≈ 2.8 G CPU cycles = 2 × dram_cycles).
4. Regression: `cmp.sh golden/step0 ubench_base` → all OK.

### Step 2 result
`phases.csv`: IDLE→INGRESS at 0, INGRESS 0–2,807,000, TRAIN 2,807,000–1,432,654,000, EGRESS 1,432,654,000–
1,435,448,000, end 1,435,449,000. cmp.sh vs golden all OK (max +0.057 %). ✅
Notes: phase stamps are multiples of 1000 cycles because ZSim's bound-weave interval is `phaseLength = 1000`
(phase change is applied at the next weave interval; error ≤ 1000 cycles ≈ 0.4 µs — negligible). Cycle 0 = ROI begin
(Ramulator does not tick during fast-forward).

---

## Step 2b — Enable DAMOV's memory clock divider (correct DDR4-2400 timing)

**Finding:** `curCycle` at the end (1,435,449,000) equals Ramulator's `dram_cycles` (1,435,449,000) exactly, i.e.
Ramulator advances **one DRAM clock per CPU clock**. DAMOV left the clock divider commented out in
`Ramulator::tick()`, so DDR4-2400 (1.2 GHz command clock) actually runs at 2.4 GHz: all DRAM timings and bandwidth are
2× too fast in real time. The log even prints `CPU/Mem frequency ratio 2`, which is computed but unused.
This affects all schedulers equally, but makes memory-bound phases (INGRESS/EGRESS, the paper's focus) look
2× cheaper and would make the Table II "DDR4-2400" claim incorrect. Fix it before collecting results.

`/DAMOV/simulator/src/ramulator_mem_ctrl.cpp`, in `Ramulator::tick()` replace
```cpp
  // REMOVE comments for clock divider (i.e., memory clock is different from host clock)
  //if((tickCounter % freqRatio) == 0){
  wrapper->tick();
  //}
  //tickCounter++;
```
with
```cpp
  // PhaSched: clock divider enabled -> DDR4-2400 runs at 1.2 GHz vs 2.4 GHz core
  if ((tickCounter % freqRatio) == 0) {
    wrapper->tick();
  }
  tickCounter++;
```
Rebuild ZSim only: `cd /DAMOV/simulator && sh scripts/compile.sh z`.

### ✅ Checks
1. Run base config. `ramulator.dram_cycles` ≈ **half** of the last `end_cycle` in `phases.csv`
   (DRAM clock = CPU clock / 2).
2. Total CPU cycles (last `end_cycle`) **increase** (memory is slower in real time); TRAIN grows most.
3. `read_latency_avg` (in DRAM clocks) stays in the same range (~50–60), so latency in ns stays ~45 ns but now
   corresponds to real time.
4. All 4 schedulers still run (FCFS, FRFCFS, FRFCFS_Cap, base).
5. Save the new reference: `golden/step2b.*` — **all later regression checks compare against step2b**.

### Step 2b result
end cycle 1,895,427,000 (was 1,435,449,000, +32 %); dram_cycles 947,713,500 = ½ of CPU cycles ✅.
read_latency_avg 55.10 DRAM clk. FCFS +4.6 % cycles / +9 % latency vs FRFCFS; FRFCFS, FRFCFS_Cap ≈ base.
New regression reference: `golden/step2b.*`.

---

## Step 3 — Per-phase metrics

**Goal:** one CSV row per (phase interval × channel) with everything the paper needs: duration, reads, read-latency
sum/P50/P99, DMA reads, writes, read↔write turnarounds, ACT/PRE/RD/WR/REF commands, row hits.
**No behaviour change** (counters only).

### 3.1 `/DAMOV/simulator/ramulator/Request.h`
Next to `int coreid = -1;` add: `bool is_dma = false;  // PhaSched: request from the NIC model (Step 6)`

### 3.2 `/DAMOV/simulator/ramulator/Controller.h` — counters
Below `int phase = 0;` add:
```cpp
    struct PhaseCnt { long reads=0, rd_lat_sum=0, dma_reads=0, writes=0, turnarounds=0,
                      act=0, pre=0, rd=0, wr=0, ref=0, row_hits=0; } pc;
    vector<long> lat_hist = vector<long>(4097, 0);  // core read latency (DRAM clk), last bin = overflow
```

### 3.3 `Controller.h` — increment the counters (inside `tick()` unless noted)
a) **Completed reads** — in `/*** 1. Serve completed reads ***/`, inside
`if (req.depart - req.arrive > 1) {`, after `(*read_latency_sum) += req.depart - req.arrive;` add:
```cpp
                  long lat = req.depart - req.arrive;
                  if (!req.is_dma) { pc.reads++; pc.rd_lat_sum += lat; lat_hist[min(lat, 4096L)]++; }
                  else pc.dma_reads++;
```
b) **Turnarounds** — replace the whole `/*** 3. Should we schedule writes? ***/` block with:
```cpp
        if (!write_mode) {
            if (writeq.size() >= int(wq_high * writeq.max) || readq.size() == 0) {
                write_mode = true; pc.turnarounds++;
            }
        }
        else {
            if (writeq.size() <= int(wq_low * writeq.max) && readq.size() != 0) {
                write_mode = false; pc.turnarounds++;
            }
        }
```
c) **Writes** — after `(*write_transaction_bytes) += tx;` add `pc.writes++;`
d) **Row hits** — after **both** `++(*row_hits);` lines (read and write branch) add `pc.row_hits++;`
e) **DRAM commands** — in `void issue_cmd(...)`, right after `assert(is_ready(cmd, addr_vec));` add:
```cpp
        {   // PhaSched per-phase command counts
            auto* sp = channel->spec;
            if (sp->is_opening(cmd)) pc.act++;
            if (sp->is_refreshing(cmd)) pc.ref++;
            if (sp->is_closing(cmd)) pc.pre++;            // PRE, PREA, RDA, WRA
            if (sp->is_accessing(cmd)) {
                if (sp->command_name[int(cmd)][0] == 'R') pc.rd++; else pc.wr++;
            }
        }
```

### 3.4 `Controller.h` — dump function (add as a public member function, e.g. just before `void finish(long dram_cycles)`)
```cpp
    void dump_phase(std::ostream& o, int ph, unsigned long long c0, unsigned long long c1) {
        long n = 0, p50 = 0, p99 = 0;
        for (int i = 0; i < (int)lat_hist.size() && pc.reads > 0; i++) {
            n += lat_hist[i];
            if (!p50 && n * 2 >= pc.reads) p50 = i;
            if (n * 100 >= pc.reads * 99) { p99 = i; break; }
        }
        o << ph << "," << c0 << "," << c1 << "," << channel->id << "," << pc.reads << "," << pc.rd_lat_sum << ","
          << p50 << "," << p99 << "," << pc.dma_reads << "," << pc.writes << "," << pc.turnarounds << ","
          << pc.act << "," << pc.pre << "," << pc.rd << "," << pc.wr << "," << pc.ref << "," << pc.row_hits << "\n";
        pc = PhaseCnt();
        std::fill(lat_hist.begin(), lat_hist.end(), 0);
    }
```

### 3.5 Plumbing (same pattern as `set_phase` in Step 2)
| File | Add |
|---|---|
| `ramulator/Memory.h`, class `MemoryBase` | `virtual void phase_snapshot(std::ostream& o, int ph, unsigned long long c0, unsigned long long c1) {}` |
| `ramulator/Memory.h`, class `Memory` (next to `set_phase`) | `void phase_snapshot(std::ostream& o, int ph, unsigned long long c0, unsigned long long c1) { for (auto ctrl : ctrls) ctrl->dump_phase(o, ph, c0, c1); }` |
| `ramulator/RamulatorWrapper.h` (public) | `void phase_snapshot(std::ostream& o, int ph, unsigned long long c0, unsigned long long c1);` (add `#include <ostream>` at top) |
| `ramulator/RamulatorWrapper.cc` | `void RamulatorWrapper::phase_snapshot(std::ostream& o, int ph, unsigned long long c0, unsigned long long c1) { mem->phase_snapshot(o, ph, c0, c1); }` |

### 3.6 `/DAMOV/simulator/src/ramulator_mem_ctrl.cpp` — write rows instead of Step 2's lines
- Constructor: header line becomes
  ```cpp
  *phaseLog << "phase,start_cycle,end_cycle,ch,reads,rd_lat_sum,rd_lat_p50,rd_lat_p99,dma_reads,writes,turnarounds,act,pre,rd,wr,ref,row_hits\n";
  ```
- `tick()` phase block: replace the `*phaseLog << "phase," ...` line with
  `wrapper->phase_snapshot(*phaseLog, curPhase, phaseStartCycle, curCycle);`
- `finish()`: replace the `*phaseLog << "end," ...` line with
  `wrapper->phase_snapshot(*phaseLog, curPhase, phaseStartCycle, curCycle);`

Cycles in columns 2–3 are CPU cycles; latencies (cols 6–8) are DRAM clocks (tCK = 0.833 ns).

### 3.7 Rebuild (headers changed → clean Ramulator) and run base config

### 3.8 Helper: `/DAMOV/phasched/scripts/phasum.sh`
```bash
#!/bin/bash
# usage: phasum.sh file.phases.csv  -> per-phase table + column sums
awk -F, 'NR==1{next}
 {d=$3-$2; printf "ph=%s ch=%s cycles=%d reads=%d avg_lat=%.1f p50=%d p99=%d dma_rd=%d wr=%d turn=%d act=%d pre=%d rd=%d wrc=%d ref=%d hits=%d\n",
   $1,$4,d,$5,($5>0?$6/$5:0),$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17;
   for(i=5;i<=17;i++) s[i]+=$i}
 END{printf "SUM reads=%d rd_lat_sum=%d dma_reads=%d writes=%d turnarounds=%d act=%d pre=%d rd=%d wr=%d ref=%d row_hits=%d\n",
   s[5],s[6],s[9],s[10],s[11],s[12],s[13],s[14],s[15],s[16],s[17]}' "$1"
```

### ✅ Checks
1. Build passes.
2. `phasum.sh ubench_base.phases.csv` shows 5 rows: IDLE (0 length), INGRESS, TRAIN, EGRESS, IDLE (final).
3. Consistency with `ramulator.stats`:
   - SUM `rd_lat_sum` == `ramulator.read_latency_sum` (exact)
   - SUM `row_hits` == `ramulator.row_hits` (exact)
   - SUM `reads` ≈ `ramulator.read_requests`, SUM `writes` ≈ `ramulator.write_requests` (within ~0.1 %; reads
     forwarded from the write queue and requests in flight at the end are not counted)
   - SUM `rd` ≈ SUM `reads`, SUM `wr` ≈ SUM `writes`; `act` ≈ `row_misses + row_conflicts`
4. Per-phase sense: INGRESS write-heavy; TRAIN read-heavy with turnarounds > 0; EGRESS mostly reads.
5. Regression: `cmp.sh golden/step2b ubench_base` all OK.

### Step 3 result
Consistency: SUM rd_lat_sum = read_latency_sum (903,443,821) exact; row_hits exact (1,057,874); writes exact
(1,310,721); reads 16,394,184 vs 16,394,193 (forwarded reads); act 16.70 M ≈ misses+conflicts 16.65 M (+0.3 %,
refresh-induced re-opens). Regression vs step2b all OK. ✅
**Problem found:** TRAIN `turnarounds = 20.9 M` (more than one per request). The counter counts `write_mode` flips,
and the baseline enters write mode whenever the read queue is empty — even with an empty write queue — so most flips
are not real bus turnarounds. → Step 3b.

## Step 3b — Count real bus turnarounds
A real turnaround = a column command whose direction differs from the previous column command (RD→WR or WR→RD);
that is when tWTR / tRTW penalties are paid.

`/DAMOV/simulator/ramulator/Controller.h`
1. Next to `PhaseCnt pc;` add: `int last_acc = -1;  // last column command: 1 = read, 0 = write`
2. In the `/*** 3. Should we schedule writes? ***/` block remove both `pc.turnarounds++;` (keep the braces).
3. In the issue_cmd counting block replace
   ```cpp
            if (sp->is_accessing(cmd)) {
                if (sp->command_name[int(cmd)][0] == 'R') pc.rd++; else pc.wr++;
            }
   ```
   with
   ```cpp
            if (sp->is_accessing(cmd)) {
                int is_rd = (sp->command_name[int(cmd)][0] == 'R');
                if (is_rd) pc.rd++; else pc.wr++;
                if (last_acc >= 0 && last_acc != is_rd) pc.turnarounds++;
                last_acc = is_rd;
            }
   ```
Clean-rebuild Ramulator, rebuild ZSim, run base.

### ✅ Checks
1. Every phase: `turn ≤ 2 × min(rd, wrc)` (a switch needs a write between reads).
2. TRAIN turnarounds now in the order of the write count (≈ 1–2.3 M), not 20.9 M.
3. All other columns and `cmp.sh` vs step2b unchanged (OK).

### Step 3b result
Turnarounds: INGRESS 10,719 · TRAIN 2,173,619 · EGRESS 12,636 — all ≤ 2 × min(rd, wr) ✅. Other columns unchanged,
cmp.sh vs step2b OK. Per-phase reference saved: `golden/step3.phases.csv`.

---

## Step 4 — PhaSched: per-phase presets + starvation guard

**Goal:** implement Paper Table I. PhaSched = FR-FCFS-Cap whose write-drain rule and row-hit cap depend on the phase.
IDLE behaves exactly like FR-FCFS-Cap (0.8 / 0.2, cap 16).

| Phase | Enter write mode | Leave write mode | Row-hit cap |
|---|---|---|---|
| 0 IDLE | baseline: wq ≥ Whi or rq = 0 | baseline: wq ≤ Wlo and rq > 0 | 16 |
| 1 INGRESS (eager) | wq > 0 | wq = 0 | unlimited |
| 2 TRAIN (lazy) | wq ≥ Whi (*forced*) or (rq = 0 and wq > 0) (*idle*) | forced: wq ≤ Wlo and rq > 0 · idle: as soon as rq > 0 · always: wq = 0 | 16 |
| 3 EGRESS | only wq ≥ Whi | wq ≤ Wlo | 64 |

Starvation guard (phases 1–3 only, so IDLE stays identical to baseline): if the oldest request of the queue being
served waited > Tmax DRAM clocks, it is served next; if the oldest request of the *other* queue waited > Tmax, switch
mode. Defaults: Tmax = 2000 DRAM clk (≈ 1.7 µs), egress cap = 64.

### 4.1 `/DAMOV/simulator/ramulator/Scheduler.h`
a) Enum: `FCFS, FRFCFS, FRFCFS_Cap, FRFCFS_PriorHit, MAX` →
   `FCFS, FRFCFS, FRFCFS_Cap, FRFCFS_PriorHit, BLISS, PhaSched, MAX`
b) In `compare[]`, after the last lambda (`// FRFCFS_PriorHit` … `return req2;}`) add a comma and two lambdas
   (order must match the enum):
```cpp
        // BLISS (placeholder = FRFCFS until Step 5)
        [this] (ReqIter req1, ReqIter req2) {
            bool ready1 = this->ctrl->is_ready(req1);
            bool ready2 = this->ctrl->is_ready(req2);
            if (ready1 ^ ready2) { if (ready1) return req1; return req2; }
            if (req1->arrive <= req2->arrive) return req1;
            return req2;},
        // PhaSched: FR-FCFS with per-phase row-hit cap
        [this] (ReqIter req1, ReqIter req2) {
            long c = this->ctrl->phase_cap();
            bool ready1 = this->ctrl->is_ready(req1) && this->ctrl->rowtable->get_hits(req1->addr_vec) <= c;
            bool ready2 = this->ctrl->is_ready(req2) && this->ctrl->rowtable->get_hits(req2->addr_vec) <= c;
            if (ready1 ^ ready2) { if (ready1) return req1; return req2; }
            if (req1->arrive <= req2->arrive) return req1;
            return req2;}
```
c) In `get_head`, as the very first statement:
```cpp
      // PhaSched starvation guard (FL phases only; queue is in arrival order)
      if (type == Type::PhaSched && this->ctrl->phase != 0 && q.size() &&
          this->ctrl->clk - q.begin()->arrive > this->ctrl->tmax)
        return q.begin();
```

### 4.2 `/DAMOV/simulator/ramulator/Controller.h`
a) `#include <climits>` at the top.
b) Members next to `wq_high / wq_low`:
```cpp
    long tmax = 2000;          // PhaSched starvation guard (DRAM clk)
    long egress_cap = 64;      // PhaSched row-hit cap in EGRESS
    bool forced_drain = false; // PhaSched TRAIN: write mode entered because of Whi
    long phase_cap() {
        if (phase == 1) return LONG_MAX;     // INGRESS: unlimited
        if (phase == 3) return egress_cap;   // EGRESS: raised
        return scheduler->cap;               // IDLE, TRAIN: baseline
    }
```
c) Constructor, Step 1 block: add `else if (sched == "PhaSched") scheduler->type = Scheduler<T>::Type::PhaSched;`
   before the final `else`, and after the other config keys:
```cpp
        if (configs["phasched_tmax"]       != "") tmax       = stol(configs["phasched_tmax"]);
        if (configs["phasched_egress_cap"] != "") egress_cap = stol(configs["phasched_egress_cap"]);
        if (sched == "PhaSched") printf("[PHASCHED] tmax=%ld egress_cap=%ld\n", tmax, egress_cap);
```
d) New member function (e.g. just before `dump_phase`):
```cpp
    void update_write_mode() {
        int wq = writeq.size(), rq = readq.size();
        int hi = int(wq_high * writeq.max), lo = int(wq_low * writeq.max);
        bool phs = (scheduler->type == Scheduler<T>::Type::PhaSched);
        if (!phs || phase == 0) {                                  // baseline FR-FCFS write drain
            if (!write_mode) { if (wq >= hi || rq == 0) write_mode = true; }
            else             { if (wq <= lo && rq != 0) write_mode = false; }
            return;
        }
        // starvation guard across queues
        if (write_mode && rq > 0 && clk - readq.q.front().arrive > tmax) { write_mode = false; return; }
        if (!write_mode && wq > 0 && clk - writeq.q.front().arrive > tmax) { write_mode = true; forced_drain = true; return; }
        if (phase == 1) {                                          // INGRESS: eager
            if (!write_mode) { if (wq > 0) write_mode = true; }
            else             { if (wq == 0) write_mode = false; }
        } else if (phase == 2) {                                   // TRAIN: lazy
            if (!write_mode) {
                if (wq >= hi)                { write_mode = true; forced_drain = true; }
                else if (rq == 0 && wq > 0)  { write_mode = true; forced_drain = false; }
            } else {
                bool leave = forced_drain ? (wq <= lo && rq != 0) : (rq != 0);
                if (leave || wq == 0) write_mode = false;
            }
        } else {                                                   // EGRESS: only at Whi
            if (!write_mode) { if (wq >= hi) write_mode = true; }
            else             { if (wq <= lo) write_mode = false; }
        }
    }
```
e) In `tick()`, replace the whole `/*** 3. Should we schedule writes? ***/` if/else block with `update_write_mode();`

### 4.2f HMC controller stub (needed to compile)
DAMOV has a separate `Controller<HMC>` (`ramulator/HMC_Controller.h`) that also uses `Scheduler<T>`, so it needs the
members the scheduler now reads. Below `bool write_mode = false;` (~line 174) in `HMC_Controller.h` add:
```cpp
    int phase = 0;                                  // PhaSched stubs (HMC not used by PhaSched)
    long tmax = 2000;
    long phase_cap() { return scheduler->cap; }
```
Build error without it: `'class ramulator::Controller<ramulator::HMC>' has no member named 'phase_cap'`.

### 4.3 Configs + no-hint test binary
```bash
cd /DAMOV/phasched/configs/ramulator
cp DDR4-FRFCFS_Cap.cfg DDR4-PhaSched.cfg
sed -i 's/scheduler = FRFCFS_Cap/scheduler = PhaSched/' DDR4-PhaSched.cfg
printf " phasched_tmax = 2000\n phasched_egress_cap = 64\n" >> DDR4-PhaSched.cfg
cd /DAMOV/phasched/configs/zsim
sed -e "s#DDR4-FRFCFS_Cap.cfg#DDR4-PhaSched.cfg#" -e "s#ubench_FRFCFS_Cap\"#ubench_PhaSched\"#" ubench_FRFCFS_Cap.cfg > ubench_PhaSched.cfg
sed -e "s#ubench_PhaSched\"#ubench_PhaSched_nohint\"#" -e "s#ubench/ubench 16#ubench/ubench_nohint 16#" ubench_PhaSched.cfg > ubench_PhaSched_nohint.cfg
grep -E "stats|command|ramulatorConfig" ubench_PhaSched.cfg ubench_PhaSched_nohint.cfg
cd /DAMOV && gcc -O2 -DPHASCHED_NO_HINTS -I phasched/include -I simulator/misc/hooks phasched/ubench/ubench.c -o phasched/ubench/ubench_nohint
chown -R 1037:1041 /DAMOV/phasched
```

### 4.4 Rebuild + 4 runs in parallel
```bash
cd /DAMOV/simulator/ramulator && make clean && make libramulator.so
cd /DAMOV/simulator && sh scripts/compile.sh z
cd /DAMOV/simulator
for c in base_ooo4_ddr4 ubench_FRFCFS_Cap ubench_PhaSched ubench_PhaSched_nohint; do
  ./build/opt/zsim ../phasched/configs/zsim/$c.cfg > ../phasched/results/$c.log 2>&1 &
done
wait
```

### ✅ Checks
1. `grep PHASCHED ../phasched/results/ubench_PhaSched.log` → `scheduler=PhaSched cap=16 …` and `tmax=2000 egress_cap=64`.
2. **Regression A:** `cmp.sh golden/step2b ubench_base` → all OK (other schedulers untouched).
3. **Regression B (key test):** `cmp.sh ubench_FRFCFS_Cap ubench_PhaSched_nohint` → all OK
   (PhaSched in IDLE == FR-FCFS-Cap).
4. **Effect:** `phasum.sh` on `ubench_FRFCFS_Cap.phases.csv` and `ubench_PhaSched.phases.csv`, compare per phase:
   INGRESS turnarounds ↓; TRAIN avg/P99 read latency ↓ or equal; EGRESS turnarounds ↓, row hits ↑ or equal.
   Note: ubench still uses the CPU as "NIC" (INGRESS mixes RFO reads with writes), so effects here are only a
   sanity check — the real INGRESS/EGRESS effect comes after the DMA injector (Step 6).
5. No deadlock: all 4 logs end with `All children done`.

### Step 4 result (ubench, CPU-proxy NIC)
- Regression A (base vs step2b) OK; **Regression B (PhaSched no-hint vs FRFCFS_Cap) OK** → IDLE == baseline ✅.
- EGRESS: turnarounds 12,810 → 1,712 (−87 %), cycles 4.06 M → 3.86 M (−4.9 %), avg read lat 35.2 → 32.3,
  P99 126 → 161 (residual writes wait until Whi), row hits +0.4 %.
- TRAIN: unchanged (ubench writes are rare; baseline already leaves write mode immediately when wq ≤ Wlo).
- INGRESS: **worse** — cycles 5.00 M → 5.62 M (+12.5 %), turnarounds 10.8 k → 35.4 k, avg lat 74.8 → 89.5.
  Cause: the CPU-proxy INGRESS (memset) mixes RFO reads with writebacks; "eager" drain switches to write mode for
  every single writeback. With a real NIC (pure DMA writes) this cannot happen → re-evaluate after the DMA injector.

---

## Step 6 (done before Step 5) — DMA injector = the NIC

Done before BLISS because the paper's INGRESS/EGRESS claims can only be tested with real NIC traffic.

**Model:** on `fl_dma_in(buf, n)` / `fl_dma_out(buf, n)` the NIC writes / reads `[buf, buf+n)` in 64 B lines directly
in DRAM (bypassing caches), one line every `interval` CPU cycles (link rate). The program spins on a poll magic op
until the last line has completed, so INGRESS/EGRESS take real simulated time.
`interval = cpuFreq · 64 B / (Gbps/8)`: 10 GbE → 122.88 cyc/line, 100 GbE → 12.29, 1000 → 1.23 (DRAM-bound).

### 6.1 App side — `/DAMOV/phasched/include/phase_hint.h`
Replace the two CPU-proxy functions `fl_dma_in` / `fl_dma_out` with:
```c
#define ZSIM_MAGIC_OP_DMA_WRITE (1041)   /* NIC -> DRAM : rdx = addr, rsi = bytes */
#define ZSIM_MAGIC_OP_DMA_READ  (1042)   /* DRAM -> NIC : rdx = addr, rsi = bytes */
#define ZSIM_MAGIC_OP_DMA_POLL  (1043)   /* rdx <- 1 while the NIC is busy */

static inline uint64_t zsim_op_ret(uint64_t op) {
    uint64_t r = 0;
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : "+d"(r) : "c"(op));
    COMPILER_BARRIER();
    return r;
}
#if defined(PHASCHED_NO_HINTS) || defined(PHASCHED_NO_DMA)
/* CPU proxy for the NIC */
static inline void fl_dma_in(void* b, size_t n)  { memset(b, 1, n); }
static inline void fl_dma_out(void* b, size_t n) {
    volatile float s = 0; const float* f = (const float*)b;
    for (size_t i = 0; i < n / sizeof(float); i += 16) s += f[i];
}
#else
static inline void fl_dma_wait(void) { while (zsim_op_ret(ZSIM_MAGIC_OP_DMA_POLL)) __builtin_ia32_pause(); }
static inline void fl_dma_in (void* b, size_t n) { zsim_op2(ZSIM_MAGIC_OP_DMA_WRITE, (uint64_t)b, n); fl_dma_wait(); }
static inline void fl_dma_out(void* b, size_t n) { zsim_op2(ZSIM_MAGIC_OP_DMA_READ,  (uint64_t)b, n); fl_dma_wait(); }
#endif
```
(`zsim_op2` must be defined above this block — it is, from Step 2. Natively the poll returns 0, so the program does
not wait.)

### 6.2 ZSim — `/DAMOV/simulator/src/zsim.cpp`
Next to `#define ZSIM_MAGIC_OP_PHASE (1040)`:
```cpp
#define ZSIM_MAGIC_OP_DMA_WRITE (1041)
#define ZSIM_MAGIC_OP_DMA_READ  (1042)
#define ZSIM_MAGIC_OP_DMA_POLL  (1043)
```
In the `switch`, after the `ZSIM_MAGIC_OP_PHASE` case:
```cpp
        case ZSIM_MAGIC_OP_DMA_WRITE:
        case ZSIM_MAGIC_OP_DMA_READ:
            if (zinfo->ramulator_memory && zinfo->ramulator) {
                uint64_t a = ((uint64_t)rdx->qword[0] >> lineBits) << lineBits;   // same mapping as cache lines
                zinfo->ramulator->requestDma(a, (uint64_t)rsi, op == ZSIM_MAGIC_OP_DMA_WRITE);
            }
            return;
        case ZSIM_MAGIC_OP_DMA_POLL:
            rdx->qword[0] = (zinfo->ramulator_memory && zinfo->ramulator && zinfo->ramulator->dmaBusy()) ? 1 : 0;
            return;
```

### 6.3 `/DAMOV/simulator/src/ramulator_mem_ctrl.h` (class Ramulator)
public:
```cpp
    void requestDma(uint64_t addr, uint64_t bytes, bool isWrite) {
      dmaReqAddr = addr; dmaReqBytes = bytes; dmaReqWrite = isWrite; dmaPending = true;
    }
    bool dmaBusy() { return dmaPending || dmaActive; }
    void setDmaLink(double gbps);
```
private:
```cpp
    volatile bool dmaPending = false, dmaActive = false;
    uint64_t dmaReqAddr = 0, dmaReqBytes = 0; bool dmaReqWrite = false;
    bool dmaWrite = false; uint64_t dmaNext = 0, dmaEnd = 0; long dmaOutstanding = 0;
    double dmaNextIssue = 0, dmaInterval = 122.88;
    std::function<void(ramulator::Request&)> dma_cb_func;
    void DRAM_dma_return_cb(ramulator::Request&);
```

### 6.4 `/DAMOV/simulator/src/ramulator_mem_ctrl.cpp`
a) Constructor body (e.g. next to the `phaseLog` lines):
```cpp
  dma_cb_func = std::bind(&Ramulator::DRAM_dma_return_cb, this, std::placeholders::_1);
```
b) New functions (end of file):
```cpp
void Ramulator::DRAM_dma_return_cb(ramulator::Request& req) { dmaOutstanding--; }

void Ramulator::setDmaLink(double gbps) {
  dmaInterval = (double)cpuFreq * 1e6 * 64.0 / (gbps * 1e9 / 8.0);
  info("[PHASCHED] DMA link %.1f Gbps -> %.2f CPU cycles per 64B line", gbps, dmaInterval);
}
```
c) In `tick()`, **after** the clock-divider block (`tickCounter++;`):
```cpp
  // ---- PhaSched DMA injector (NIC) ----
  if (dmaPending && !dmaActive) {
    dmaWrite = dmaReqWrite; dmaNext = dmaReqAddr;
    dmaEnd = dmaReqAddr + ((dmaReqBytes + 63) / 64) * 64;
    dmaOutstanding = 0; dmaNextIssue = curCycle;
    dmaActive = true; dmaPending = false;
  }
  if (dmaActive) {
    if (dmaNext < dmaEnd && curCycle >= dmaNextIssue) {
      ramulator::Request req((long)dmaNext,
          dmaWrite ? ramulator::Request::Type::WRITE : ramulator::Request::Type::READ, dma_cb_func, 0);
      req.is_dma = true;
      if (wrapper->send(req)) { dmaNext += 64; dmaOutstanding++; dmaNextIssue += dmaInterval; }
    }
    if (dmaNext >= dmaEnd && dmaOutstanding == 0) dmaActive = false;
  }
```
(If the controller queue is full, `send` fails and the line is retried next cycle; the NIC then catches up —
models the NIC's on-board buffer.)

### 6.5 `/DAMOV/simulator/src/init.cpp`
After `zinfo -> ramulator = static_cast<Ramulator*>(mem);` add:
```cpp
        zinfo->ramulator->setDmaLink(config.get<double>("sys.mem.dmaLinkGbps", 10.0));
```

### 6.6 Rebuild + test configs
Only ZSim files changed (no Ramulator header) → `sh scripts/compile.sh z`. Rebuild `ubench` (DMA version) with
the Step 0.3 command (`ubench_nohint` stays the CPU-proxy build).
Test configs: copies of `ubench_FRFCFS_Cap.cfg` / `ubench_PhaSched.cfg` with `dmaLinkGbps = <X>;` added inside
`mem = { ... }` and stats names `ubench_<sched>_<X>g`, for X = 10 and 1000.

### ✅ Checks
1. Log shows `[PHASCHED] DMA link 10.0 Gbps -> 122.88 CPU cycles per 64B line`.
2. 10 GbE: INGRESS and EGRESS each ≈ 16 MiB / 1.25 GB/s = 13.42 ms = **32.2 M cycles (±2 %)**.
   INGRESS `wr` ≈ 262,144 and `reads` ≈ 0; EGRESS `dma_rd` ≈ 262,144.
3. 1000 Gbps: INGRESS/EGRESS DRAM-bound, ≥ 16 MiB / 19.2 GB/s = 0.87 ms ≈ **2.1 M cycles** (expect ~2.5–4 M).
4. No hang (all logs end with `All children done`); native `./phasched/ubench/ubench 16 64 1` still runs.
5. PhaSched vs FRFCFS_Cap at both rates: per-phase cycles, turnarounds, row hits.

### Step 6 result (ubench, 16 MiB model)
| | INGRESS cycles | EGRESS cycles | INGRESS core-read avg lat (5 reads) |
|---|---|---|---|
| FRFCFS_Cap 10 GbE | 32,213,000 | 32,213,000 | 26.8 |
| PhaSched 10 GbE | 32,213,000 (identical) | 32,213,000 (identical) | 26.8 |
| FRFCFS_Cap 1000 Gbps | 5,957,000 | 5,940,000 | **297,850** (reads starved) |
| PhaSched 1000 Gbps | **2,779,000 (−53 %)** | **5,542,000 (−6.7 %)** | 9,415 |
- Injector validated: 10 GbE INGRESS/EGRESS = 32.2 M cycles exactly as predicted; 262,144 DMA writes / reads.
- 10 GbE: PhaSched ≡ FRFCFS_Cap → link-bound, scheduler cannot help (risk confirmed).
- 1000 Gbps: baseline reaches only 16 MiB / 5.96 M cyc = 6.75 GB/s for a pure write stream; PhaSched 14.5 GB/s
  (76 % of 19.2 GB/s peak). → at 100 GbE (12.5 GB/s) the baseline is DRAM-bound but PhaSched is not.
- Bug: PhaSched's starvation guard does not hold read mode — in INGRESS the eager rule flips back to write mode one
  cycle later, so a starved read (needs PRE/ACT/RD) still waits ~9 k clk instead of ~Tmax.

## Step 6b — Fix starvation guard + attribution runs
`Controller.h`, `update_write_mode()`: replace the two "starvation guard across queues" lines with
```cpp
        // starvation guard across queues: hold the mode until the starved request is served
        bool rstarve = rq > 0 && clk - readq.q.front().arrive > tmax;
        bool wstarve = wq > 0 && clk - writeq.q.front().arrive > tmax;
        if (rstarve) { write_mode = false; return; }
        if (wstarve) { write_mode = true; forced_drain = true; return; }
```
Clean-rebuild Ramulator + ZSim.

Runs (6 in parallel): FRFCFS_1000g, FCFS_1000g (attribution: is the INGRESS gain from the cap or the drain rule?),
PhaSched_1000g (guard fix), FRFCFS_Cap_100g, PhaSched_100g (realistic fast link), PhaSched_10g (regression).

### ✅ Checks
1. PhaSched_1000g INGRESS core-read avg latency ≈ Tmax range (≤ ~2,100 clk), INGRESS cycles still ≈ 2.8 M.
2. PhaSched_10g identical to before (32.2 M / 32.2 M).
3. FRFCFS_1000g vs FRFCFS_Cap_1000g INGRESS → shows how much of the gain is the lifted cap.
4. 100 GbE: expected INGRESS ≈ 16 MiB / 12.5 GB/s = 1.34 ms = 3.22 M cycles for PhaSched; baseline slower
   (DRAM-bound at ~6.75 GB/s → ~5.96 M).
