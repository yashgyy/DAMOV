#!/bin/bash
# OCCARB Step 6 checks: source arbitration policies FRFCFS / CoreFirst / DMAFirst.
# Run from simulator/ AFTER bash occarb/rebuild.sh:  bash occarb/step6/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step6/out; rm -rf $OUT; mkdir -p $OUT
CMD="$WL/STREAM/stream_copy 1"; INSTRS=20000000L
absok(){ awk "BEGIN{d=($1-$2)/$2; if(d<0)d=-d; exit !(d<=$3)}"; }

# 6a-6f: scheduler unit tests (Ramulator only, through RamulatorWrapper)
if g++ -std=c++11 -DRAMULATOR -w -I ramulator occarb/step6/test_sched.cc -L ramulator -lramulator \
       -Wl,-rpath,"$SIM/ramulator" -o $OUT/test_sched 2> $OUT/build.log; then
  $OUT/test_sched "$SIM/ramulator-configs/DDR4-occarb.cfg" $OUT 2>/dev/null | grep -E "^(PASS|FAIL|ALL|[0-9]+ FAILED)"
  [ ${PIPESTATUS[0]} = 0 ] || FAILED=1
else fail "unit test did not compile (see $OUT/build.log)"; cat $OUT/build.log; fi

# 6g: regressions
bash occarb/step4/run_check.sh > $OUT/step4.txt 2>&1 && pass "6g Step 4 NIC unit tests" || fail "6g Step 4 (see $OUT/step4.txt)"
REF1=26616590
bash occarb/step1/run_check.sh > $OUT/step1.txt 2>&1
C1=$(grep -m1 'core cycles' $OUT/step1.txt | awk '{print $4}')
grep -q "PASS 1d" $OUT/step1.txt && absok $C1 $REF1 0.005 && pass "6g Step 1 within 0.5% ($C1 vs $REF1)" || fail "6g Step 1: $C1 vs $REF1"

# mk NAME POLICY [nic key value]...
mk(){ local d=$OUT/$1 pol=$2; shift 2; mkdir -p $d; ram_policy $d $pol
      gen_cfg $d/run.cfg 1 r "$CMD" $INSTRS ramulator-configs/DDR4-occarb.cfg
      while [ $# -gt 0 ]; do nic_set $d/run.cfg $1 $2; shift 2; done
      rc=$(run_dir $d); [ "$rc" = 0 ] && [ -f $d/r.zsim.out ] || fail "run $(basename $d) failed (see $d/zsim.log)"; }
T(){ max_cycles $OUT/$1 r; }
Z(){ zstat $OUT/$1 r $2; }

for p in FRFCFS CoreFirst DMAFirst; do
  mk off_$p $p enable false
  mk on_$p  $p enable true linkGbps 25.0
done
for p in FRFCFS CoreFirst DMAFirst; do
  grep -q "src_policy=$p" $OUT/on_$p/zsim.log && pass "6h $p: policy active in Ramulator log" || fail "6h $p: '$p' not in log"
done

# 6i: without NIC traffic the policy is irrelevant -> identical cycles
T0=$(T off_FRFCFS)
[ "$(T off_CoreFirst)" = "$T0" ] && [ "$(T off_DMAFirst)" = "$T0" ] && pass "6i NIC off: identical cycles under all policies ($T0)" \
  || fail "6i NIC off: FR=$T0 CF=$(T off_CoreFirst) DF=$(T off_DMAFirst)"

# 6j: RX+TX 25 GbE: slowdown and DMA latency ordering
for p in FRFCFS CoreFirst DMAFirst; do
  S=$(awk "BEGIN{printf \"%.4f\", $(T on_$p)/$T0}")
  RL=$(awk "BEGIN{printf \"%.0f\", $(Z on_$p nicRxDmaLat)/$(Z on_$p nicRxDmaWrites)}")
  TL=$(awk "BEGIN{printf \"%.0f\", $(Z on_$p nicTxDmaLat)/$(Z on_$p nicTxDmaReads)}")
  LOSS=$(awk "BEGIN{printf \"%.4f\", $(Z on_$p nicRxPktsDropped)/$(Z on_$p nicRxPktsArrived)}")
  L3=$(awk "BEGIN{printf \"%.4f\", $(Z on_$p nicLevel3Cycles)/$(Z on_$p nicCycles)}")
  eval S_$p=$S RL_$p=$RL
  printf "     %-9s S=%s  rxDmaLat=%s  txDmaLat=%s  loss=%s  underruns=%s  timeAtLevel3=%s\n" $p $S $RL $TL $LOSS "$(Z on_$p nicTxUnderruns)" $L3
done
awk "BEGIN{exit !($S_CoreFirst <= $S_FRFCFS && $S_FRFCFS <= $S_DMAFirst)}" \
  && pass "6j slowdown ordering CoreFirst ($S_CoreFirst) <= FRFCFS ($S_FRFCFS) <= DMAFirst ($S_DMAFirst)" \
  || fail "6j slowdown ordering: CF=$S_CoreFirst FR=$S_FRFCFS DF=$S_DMAFirst"
awk "BEGIN{exit !($RL_DMAFirst <= $RL_FRFCFS && $RL_FRFCFS <= $RL_CoreFirst)}" \
  && pass "6j DMA latency ordering DMAFirst ($RL_DMAFirst) <= FRFCFS ($RL_FRFCFS) <= CoreFirst ($RL_CoreFirst)" \
  || fail "6j DMA latency ordering: DF=$RL_DMAFirst FR=$RL_FRFCFS CF=$RL_CoreFirst"
exit $FAILED
