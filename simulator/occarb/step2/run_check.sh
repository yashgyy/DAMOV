#!/bin/bash
# OCCARB Step 2 checks. Run from simulator/:  bash occarb/step2/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step2/out; rm -rf $OUT; mkdir -p $OUT
CMD="$WL/STREAM/stream_copy 4"; INSTRS=40000000L

# 2a: Step 1 regression (queue_size absent -> default 32 -> identical to Step 1)
bash occarb/step1/run_check.sh > $OUT/step1_regress.txt 2>&1
C1=$(grep -m1 'core cycles' $OUT/step1_regress.txt | awk '{print $4}')
[ "$C1" = "26637522" ] && pass "2a Step 1 unchanged after Controller.h edit ($C1)" \
  || fail "2a Step 1 cycles changed: $C1 (expected 26637522), see $OUT/step1_regress.txt"

# 2b: Table II run (4 cores, DDR4-occarb, queue 64)
D=$OUT/q64; mkdir -p $D
gen_cfg $D/run.cfg 4 base "$CMD" $INSTRS ramulator-configs/DDR4-occarb.cfg
rc=$(run_dir $D)
grep -q "Injector exited" $D/zsim.log && fail "2b Pin injector crashed"
[ "$rc" = 0 ] && [ -f $D/base.zsim.out ] && pass "2b Table II run completed" || fail "2b rc=$rc, see $D/zsim.log"

# 2c: configuration actually applied
N=$(ncores $D base); [ "$N" = 4 ] && pass "2c 4 cores in zsim.out" || fail "2c cores=$N (expected 4)"
for c in "l1d. has 32768" "l1i. has 32768" "l2. has 262144" "l3. has 8388608"; do
  grep -q "$c" $D/zsim.log && pass "2c cache: $c" || fail "2c cache line missing: $c"
done
grep -q "Mem frequency 1200" $D/zsim.log && pass "2c DDR4-2400 (mem freq 1200 MHz)" \
  || fail "2c mem freq: $(grep -m1 'Mem frequency' $D/zsim.log)"
grep -q "queue depth: readq=64 writeq=64" $D/zsim.log && pass "2c controller queue depth = 64" \
  || fail "2c queue depth line: $(grep -m1 'queue depth' $D/zsim.log)"
R=$(rstat $D base read_requests); W=$(rstat $D base write_requests)
[ -n "$R" ] && awk "BEGIN{exit !($R>0)}" && pass "2c DRAM traffic (reads=$R writes=$W)" || fail "2c no DRAM reads"

# 2d: determinism of the 4-core baseline
D2=$OUT/q64_rep; mkdir -p $D2; cp $D/run.cfg $D2/; run_dir $D2 >/dev/null
T1=$(max_cycles $D base); T2=$(max_cycles $D2 base)
[ "$T1" = "$T2" ] && pass "2d deterministic 4-core run ($T1)" \
  || echo "WARN 2d 4-core cycles differ ($T1 vs $T2, $(awk "BEGIN{printf \"%.3f%%\", 100*($T2-$T1)/$T1}"))"

# 2e: knob is live (queue 32 vs 64 must change timing)
D3=$OUT/q32; mkdir -p $D3/ramulator-configs
sed 's/^ queue_size = 64/ queue_size = 32/' ramulator-configs/DDR4-occarb.cfg > $D3/ramulator-configs/DDR4-occarb.cfg
cp $D/run.cfg $D3/; run_dir $D3 >/dev/null
T3=$(max_cycles $D3 base)
grep -q "readq=32 writeq=32" $D3/zsim.log && [ "$T3" != "$T1" ] \
  && pass "2e queue_size knob changes timing (q32=$T3 vs q64=$T1)" || fail "2e q32=$T3 q64=$T1"

echo "T_alone (STREAM Copy x4, ${INSTRS}) = $T1 cycles"
exit $FAILED
