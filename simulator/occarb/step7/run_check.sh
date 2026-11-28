#!/bin/bash
# OCCARB Step 7 checks: occupancy-driven arbitration (correctness). Run from simulator/ AFTER bash occarb/rebuild.sh:
#   bash occarb/step7/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step7/out; rm -rf $OUT; mkdir -p $OUT
CMD="$WL/STREAM/stream_copy 1"; INSTRS=20000000L
absok(){ awk "BEGIN{d=($1-$2)/$2; if(d<0)d=-d; exit !(d<=$3)}"; }

# 7a-7d: OCCARB unit tests (Ramulator only)
if g++ -std=c++11 -DRAMULATOR -w -I ramulator occarb/step7/test_occarb.cc -L ramulator -lramulator \
       -Wl,-rpath,"$SIM/ramulator" -o $OUT/test_occarb 2> $OUT/build.log; then
  $OUT/test_occarb "$SIM/ramulator-configs/DDR4-occarb.cfg" $OUT 2>/dev/null | grep -E "^(PASS|FAIL|ALL|[0-9]+ FAILED)"
  [ ${PIPESTATUS[0]} = 0 ] || FAILED=1
else fail "unit test did not compile (see $OUT/build.log)"; cat $OUT/build.log; fi

# 7e: regressions
bash occarb/step4/run_check.sh > $OUT/step4.txt 2>&1 && pass "7e Step 4 NIC unit tests" || fail "7e Step 4 (see $OUT/step4.txt)"
g++ -std=c++11 -DRAMULATOR -w -I ramulator occarb/step6/test_sched.cc -L ramulator -lramulator -Wl,-rpath,"$SIM/ramulator" -o $OUT/test_sched 2>/dev/null \
  && $OUT/test_sched "$SIM/ramulator-configs/DDR4-occarb.cfg" $OUT > $OUT/step6_unit.txt 2>&1 \
  && pass "7e Step 6 scheduler unit tests" || fail "7e Step 6 unit tests (see $OUT/step6_unit.txt)"
REF1=26459523
bash occarb/step1/run_check.sh > $OUT/step1.txt 2>&1
C1=$(grep -m1 'core cycles' $OUT/step1.txt | awk '{print $4}')
grep -q "PASS 1d" $OUT/step1.txt && absok $C1 $REF1 0.005 && pass "7e Step 1 within 0.5% ($C1 vs $REF1)" || fail "7e Step 1: $C1 vs $REF1"

# mk NAME POLICY "ramkey=val ..." [nic key value]...
mk(){ local d=$OUT/$1 pol=$2 rk=$3; shift 3; mkdir -p $d; ram_policy $d $pol
      for kv in $rk; do ram_set $d ${kv%%=*} ${kv#*=}; done
      gen_cfg $d/run.cfg 1 r "$CMD" $INSTRS ramulator-configs/DDR4-occarb.cfg
      while [ $# -gt 0 ]; do nic_set $d/run.cfg $1 $2; shift 2; done
      rc=$(run_dir $d); [ "$rc" = 0 ] && [ -f $d/r.zsim.out ] || fail "run $(basename $d) failed (see $d/zsim.log)"; }
T(){ max_cycles $OUT/$1 r; }
Z(){ zstat $OUT/$1 r $2; }
R(){ rstat $OUT/$1 r $2; }

NIC="enable true linkGbps 25.0"
mk off      FRFCFS ""                                         enable false
mk cf       CoreFirst ""                                      $NIC
mk df       DMAFirst ""                                       $NIC
mk fr       FRFCFS ""                                         $NIC
mk oc       OCCARB "occarb_theta_lo=0.25 occarb_theta_hi=0.75" $NIC
mk oc_R     OCCARB "occarb_theta_lo=1.0 occarb_theta_hi=1.0"   $NIC
mk oc_U     OCCARB "occarb_theta_lo=0.0 occarb_theta_hi=0.0"   $NIC
mk oc_win   OCCARB "occarb_theta_lo=0.25 occarb_theta_hi=0.75" $NIC rho 0.5 periodCycles 2000000
T0=$(T off)

# 7f: configuration reaches Ramulator
grep -q "OCCARB: theta_lo=0.2500 theta_hi=0.7500 source=nic" $OUT/oc/zsim.log && pass "7f OCCARB parameters in log" || fail "7f OCCARB log line"

# 7g: integration equals the static policies at degenerate thresholds (sideband plumbing + mode mapping)
absok $(T oc_R) $(T cf) 0.02 && pass "7g OCCARB(lo=hi=1) ~ CoreFirst within 2% run-to-run noise ($(T oc_R) vs $(T cf))" || fail "7g oc_R $(T oc_R) vs cf $(T cf)"
absok $(T oc_U) $(T df) 0.02 && pass "7g OCCARB(lo=hi=0) ~ DMAFirst within 2% run-to-run noise ($(T oc_U) vs $(T df))" || fail "7g oc_U $(T oc_U) vs df $(T df)"

# 7h: the NIC sideband drives the modes; residency accounting
for r in oc oc_win; do
  RC=$(R $r occarb_relaxed_cycles_0); NC=$(R $r occarb_neutral_cycles_0); UC=$(R $r occarb_urgent_cycles_0)
  TR=$(R $r occarb_transitions_0); DC=$(R $r dram_cycles)
  echo "     $r: RELAXED=$RC NEUTRAL=$NC URGENT=$UC transitions=$TR dram_cycles=$DC"
  absok $((RC+NC+UC)) $DC 0.001 && pass "7h $r: mode residency sums to DRAM cycles" || fail "7h $r: $((RC+NC+UC)) vs $DC"
done
[ "$(R oc_win occarb_transitions_0)" -ge 2 ] && [ "$(R oc_win occarb_urgent_cycles_0)" -gt 0 ] \
  && pass "7h windowed link (rho=0.5): sideband drives mode changes incl. URGENT" || fail "7h no mode activity with rho=0.5"

# 7i: OCCARB result on 1 core (expected between CoreFirst and DMAFirst; no loss)
for r in cf fr oc df; do
  printf "     %-3s S=%s  loss=%s  rxDmaLat=%s\n" $r $(awk "BEGIN{printf \"%.4f\", $(T $r)/$T0}") \
    $(awk "BEGIN{printf \"%.4f\", $(Z $r nicRxPktsDropped)/$(Z $r nicRxPktsArrived)}") $(awk "BEGIN{printf \"%.0f\", $(Z $r nicRxDmaLat)/$(Z $r nicRxDmaWrites)}")
done
awk "BEGIN{exit !($(T cf)*0.98 <= $(T oc) && $(T oc) <= $(T df)*1.02)}" && pass "7i S(OCCARB) within [S(CoreFirst), S(DMAFirst)] +-2% noise" || fail "7i OCCARB outside [CoreFirst, DMAFirst]"
[ "$(Z oc nicRxPktsDropped)" = 0 ] && pass "7i OCCARB: zero packet loss" || fail "7i OCCARB dropped $(Z oc nicRxPktsDropped)"
exit $FAILED
