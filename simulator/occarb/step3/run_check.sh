#!/bin/bash
# OCCARB Step 3 checks. Run from simulator/ AFTER bash occarb/rebuild.sh:
#   bash occarb/step3/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step3/out; rm -rf $OUT; mkdir -p $OUT

# 3a: the new field is in the header that both Ramulator and ZSim compile against
grep -q "bool is_dma = false;" ramulator/Request.h && pass "3a Request.h has is_dma" || fail "3a is_dma missing in Request.h"
[ ramulator/libramulator.so -nt ramulator/Request.h ] && [ build/opt/libzsim.so -nt ramulator/Request.h ] \
  && pass "3a libramulator.so and libzsim.so rebuilt after the edit" || fail "3a rebuild needed: bash occarb/rebuild.sh"

# 3b: unit test (no simulator)
if g++ -std=c++11 -I ramulator occarb/step3/test_request.cc -o $OUT/test_request 2> $OUT/test_build.log; then
  $OUT/test_request || FAILED=1
else fail "3b unit test did not compile (see $OUT/test_build.log)"; fi

# 3c: Step 1 regression. No logic change expected; absolute cycles may move slightly because
#     code-size changes shift memory layout (see PROGRESS.md Step 2). Tolerance 0.5%.
REF1=26587526
bash occarb/step1/run_check.sh > $OUT/step1_regress.txt 2>&1
C1=$(grep -m1 'core cycles' $OUT/step1_regress.txt | awk '{print $4}')
grep -q "PASS 1d" $OUT/step1_regress.txt && pass "3c Step 1 deterministic" || fail "3c Step 1 not deterministic"
D=$(awk "BEGIN{d=($C1-$REF1)/$REF1*100; if(d<0)d=-d; printf \"%.3f\", d}")
if [ "$C1" = "$REF1" ]; then pass "3c Step 1 bit-identical ($C1)"
elif awk "BEGIN{exit !($D<0.5)}"; then pass "3c Step 1 within 0.5% of reference ($C1 vs $REF1, ${D}%) -> layout shift only"
else fail "3c Step 1 changed by ${D}% ($C1 vs $REF1)"; fi
echo "New Step 1 reference for this build: $C1"
exit $FAILED
