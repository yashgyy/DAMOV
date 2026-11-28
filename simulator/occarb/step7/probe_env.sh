#!/bin/bash
# Probe: does fixing the OpenMP thread pool (OMP_NUM_THREADS = cores, OMP_WAIT_POLICY=ACTIVE) make runs repeatable?
# No source changes: only adds an env line to copies of existing configs. Run from simulator/:
#   bash occarb/step7/probe_env.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
P=occarb/step7/probe_env; rm -rf $P; mkdir -p $P
addenv(){ sed -i "s|^\(\s*command = .*\)\$|\1\n    env = \"OMP_NUM_THREADS=$2 OMP_WAIT_POLICY=ACTIVE\";|" "$1"; }
cyc(){ grep -hE '^\s+cycles:' $1/*.zsim.out 2>/dev/null | awk '{if($2>m)m=$2}END{print m+0}'; }

echo "== A: Step 1 smoke (1 core, no NIC) with env, 3 runs"
for i in 1 2 3; do d=$P/smoke_$i; mkdir -p $d
  sed "s#WORKLOADS_ROOT#$WL#" occarb/step1/smoke.cfg > $d/run.cfg; addenv $d/run.cfg 1
  run_dir $d >/dev/null; echo "   run $i: $(cyc $d)"; done
grep -m1 "env = " $P/smoke_1/run.cfg

echo "== B: OCCARB(theta=1) x2 and CoreFirst x2, 1 core, 25 GbE, with env"
for n in oc_R cf; do for i in 1 2; do d=$P/${n}_$i; mkdir -p $d
  cp -r occarb/step7/out/$n/run.cfg occarb/step7/out/$n/ramulator-configs $d/; addenv $d/run.cfg 1
  run_dir $d >/dev/null; echo "   $n run $i: $(cyc $d)"; done; done

echo "== C: 4-core STREAM, no NIC, with env (OMP_NUM_THREADS=4), 3 runs"
for i in 1 2 3; do d=$P/four_$i; mkdir -p $d
  gen_cfg $d/run.cfg 4 r "$WL/STREAM/stream_copy 4" 40000000L ramulator-configs/DDR4-occarb.cfg; addenv $d/run.cfg 4
  run_dir $d >/dev/null; echo "   run $i: $(cyc $d)  row_conflicts=$(rstat $d r row_conflicts)"; done
