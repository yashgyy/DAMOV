#!/bin/bash
# OCCARB Step 5a checks: optional CPU/DRAM clock divider.
# Run from simulator/ AFTER bash occarb/rebuild.sh:  bash occarb/step5a/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step5a/out; rm -rf $OUT; mkdir -p $OUT
CMD="$WL/STREAM/stream_copy 4"; INSTRS=40000000L
PEAK=19.2   # GB/s: DDR4-2400 x 64-bit bus = 2400e6 * 8 B

# a1: default (no clockDivide key) = original DAMOV behaviour -> Step 1 unchanged (0.5% layout tolerance)
REF1=26587526
bash occarb/step1/run_check.sh > $OUT/step1_regress.txt 2>&1
C1=$(grep -m1 'core cycles' $OUT/step1_regress.txt | awk '{print $4}')
D=$(awk "BEGIN{d=($C1-$REF1)/$REF1*100; if(d<0)d=-d; printf \"%.3f\", d}")
grep -q "PASS 1d" $OUT/step1_regress.txt && awk "BEGIN{exit !($D<0.5)}" \
  && pass "5a-1 Step 1 default path unchanged ($C1 vs $REF1, ${D}%)" || fail "5a-1 Step 1: $C1 vs $REF1 (${D}%)"
grep -q "Clock divider OFF" occarb/step1/out_run1/zsim.log && pass "5a-1 default reports divider OFF" \
  || fail "5a-1 'Clock divider OFF' not in Step 1 log"

# a2: 4-core STREAM with divider OFF vs ON
for mode in off on; do
  d=$OUT/$mode; mkdir -p $d
  gen_cfg $d/run.cfg 4 base "$CMD" $INSTRS ramulator-configs/DDR4-occarb.cfg
  [ $mode = off ] && sed -i 's/clockDivide = true;/clockDivide = false;/' $d/run.cfg
  rc=$(run_dir $d)
  [ "$rc" = 0 ] && [ -f $d/base.zsim.out ] || fail "5a-2 run '$mode' failed (see $d/zsim.log)"
done
grep -q "Clock divider ON (DRAM ticks every 2 CPU cycles)" $OUT/on/zsim.log && pass "5a-2 ON: DRAM ticks every 2 CPU cycles" \
  || fail "5a-2 ON log line: $(grep -m1 'Clock divider' $OUT/on/zsim.log)"
grep -q "Clock divider OFF" $OUT/off/zsim.log && pass "5a-2 OFF: divider off" || fail "5a-2 OFF log line missing"

for mode in off on; do
  d=$OUT/$mode
  T=$(max_cycles $d base); R=$(rstat $d base read_requests); W=$(rstat $d base write_requests); DC=$(rstat $d base dram_cycles)
  BW=$(awk "BEGIN{printf \"%.2f\", ($R+$W)*64/($T/2.4e9)/1e9}")
  RATIO=$(awk "BEGIN{printf \"%.3f\", $T/$DC}")
  eval T_$mode=$T BW_$mode=$BW RATIO_$mode=$RATIO
  echo "     $mode: cycles=$T reads=$R writes=$W dram_cycles=$DC cpu/dram=$RATIO bandwidth=${BW} GB/s"
done
awk "BEGIN{exit !($RATIO_on>1.95 && $RATIO_on<2.05)}" && pass "5a-2 ON: cpu cycles / dram cycles ~ 2 ($RATIO_on)" || fail "5a-2 ON ratio $RATIO_on"
awk "BEGIN{exit !($RATIO_off>0.95 && $RATIO_off<1.05)}" && pass "5a-2 OFF: cpu cycles / dram cycles ~ 1 ($RATIO_off)" || fail "5a-2 OFF ratio $RATIO_off"
awk "BEGIN{exit !($BW_on<=$PEAK)}" && pass "5a-2 ON: bandwidth ${BW_on} GB/s <= DDR4-2400 peak $PEAK GB/s" || fail "5a-2 ON bandwidth ${BW_on} > peak"
[ "$T_on" -gt "$T_off" ] && pass "5a-2 ON is slower than OFF ($T_on > $T_off, x$(awk "BEGIN{printf \"%.3f\", $T_on/$T_off}"))" \
  || fail "5a-2 ON not slower ($T_on vs $T_off)"
echo "New T_alone (STREAM Copy x4, ${INSTRS}, clockDivide ON) = $T_on cycles"
exit $FAILED
