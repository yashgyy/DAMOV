#!/bin/bash
# OCCARB Step 7 stress experiment (4 cores, memory-saturating): the paper's claim in miniature.
# Reports findings (INFO), not correctness. Run from simulator/:  bash occarb/step7/stress.sh [REPEATS] [BUF_KB]
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
REP=${1:-3}; BUF=${2:-256}
OUT=occarb/step7/stress_out; rm -rf $OUT; mkdir -p $OUT
CMD="$WL/STREAM/stream_copy 4"; INSTRS=40000000L
mk(){ local d=$OUT/$1 pol=$2 rk=$3; shift 3; mkdir -p $d; ram_policy $d $pol
      for kv in $rk; do ram_set $d ${kv%%=*} ${kv#*=}; done
      gen_cfg $d/run.cfg 4 r "$CMD" $INSTRS ramulator-configs/DDR4-occarb.cfg
      while [ $# -gt 0 ]; do nic_set $d/run.cfg $1 $2; shift 2; done
      run_dir $d >/dev/null; }
NIC="enable true linkGbps 25.0 bufKB $BUF"
for i in $(seq 1 $REP); do
  echo "repeat $i/$REP ..."
  mk off_$i FRFCFS "" enable false
  mk fr_$i  FRFCFS "" $NIC
  mk cf_$i  CoreFirst "" $NIC
  mk df_$i  DMAFirst "" $NIC
  mk oc_$i  OCCARB "occarb_theta_lo=0.25 occarb_theta_hi=0.75" $NIC
done
T0=$(for i in $(seq 1 $REP); do max_cycles $OUT/off_$i r; done | awk '{s+=$1}END{print s/NR}')
echo "T_alone (mean of $REP) = $T0 cycles;  buffer = $BUF KB per direction, 25 GbE RX+TX, STREAM x4"
printf "%-9s %-22s %-10s %-10s %-10s\n" policy "S mean [min,max]" "loss" "underruns" "URGENT%"
for p in fr cf df oc; do
  S=$(for i in $(seq 1 $REP); do echo "$(max_cycles $OUT/${p}_$i r) $T0"; done | awk '{v=$1/$2; s+=v; if(NR==1||v<mn)mn=v; if(v>mx)mx=v}END{printf "%.4f [%.4f,%.4f]", s/NR, mn, mx}')
  L=$(for i in $(seq 1 $REP); do echo "$(zstat $OUT/${p}_$i r nicRxPktsDropped) $(zstat $OUT/${p}_$i r nicRxPktsArrived)"; done | awk '{d+=$1; a+=$2}END{printf "%.4f", d/a}')
  U=$(for i in $(seq 1 $REP); do zstat $OUT/${p}_$i r nicTxUnderruns; done | awk '{s+=$1}END{print s}')
  UR="-"; [ $p = oc ] && UR=$(for i in $(seq 1 $REP); do echo "$(rstat $OUT/oc_$i r occarb_urgent_cycles_0) $(rstat $OUT/oc_$i r dram_cycles)"; done | awk '{u+=$1; c+=$2}END{printf "%.2f", 100*u/c}')
  printf "%-9s %-22s %-10s %-10s %-10s\n" $p "$S" $L $U $UR
done
