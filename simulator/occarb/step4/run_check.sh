#!/bin/bash
# OCCARB Step 4 checks: NIC model unit tests (no simulator, no rebuild needed).
# Run from simulator/:  bash occarb/step4/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step4/out; rm -rf $OUT; mkdir -p $OUT
# Not yet included by the simulator -> simulator behaviour cannot have changed.
N=$(grep -rl "NicModel.h" src ramulator --include=*.cpp --include=*.cc --include=*.h | grep -v "ramulator/NicModel.h" | wc -l)
[ "$N" = 0 ] && pass "4- NicModel.h not yet included by ZSim/Ramulator (no rebuild needed)" || echo "NOTE NicModel.h included in $N file(s)"
if g++ -std=c++11 -Wall -Wextra -I ramulator occarb/step4/test_nic.cc -o $OUT/test_nic 2> $OUT/build.log; then
  [ -s $OUT/build.log ] && { echo "WARN compiler warnings:"; cat $OUT/build.log; }
  $OUT/test_nic || FAILED=1
else fail "unit tests did not compile (see $OUT/build.log)"; cat $OUT/build.log; fi
exit $FAILED
