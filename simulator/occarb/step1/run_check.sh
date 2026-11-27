#!/bin/bash
# OCCARB Step 1 checks. Run from simulator/:  bash occarb/step1/run_check.sh
set -u
SIM=$(cd "$(dirname "$0")/../.." && pwd); cd "$SIM"
pass(){ echo "PASS $1"; }; fail(){ echo "FAIL $1"; FAILED=1; }; FAILED=0

# 1a: build artifacts
[ -x build/opt/zsim ] && pass "1a zsim binary" || fail "1a build/opt/zsim missing"
[ -f ramulator/libramulator.so ] && pass "1a libramulator.so" || fail "1a libramulator.so missing"

cycles(){ grep -m1 -E '^\s+cycles:' "$1/smoke.zsim.out" | awk '{print $2}'; }
run(){ rm -rf "$1"; mkdir -p "$1"; (cd "$1" && ln -sf "$SIM/ramulator-configs" . && \
       "$SIM/build/opt/zsim" "$SIM/occarb/step1/smoke.cfg" > zsim.log 2>&1); echo $?; }

# 1b: run completes
R1=occarb/step1/out_run1
rc=$(run $R1)
[ "$rc" = 0 ] && [ -f $R1/smoke.zsim.out ] && pass "1b smoke run (rc=0, smoke.zsim.out)" || fail "1b rc=$rc, see $R1/zsim.log"

# 1c: non-zero activity in core and in Ramulator
C1=$(cycles $R1); echo "     core cycles = ${C1:-none}"
[ -n "${C1:-}" ] && [ "$C1" -gt 0 ] && pass "1c core cycles > 0" || fail "1c no core cycles"
RS=$(ls $R1/*.ramulator.stats 2>/dev/null | head -1)
RD=$( [ -n "$RS" ] && grep -E 'numberofreads_s_0' "$RS" | awk '{print $2}')
echo "     ramulator reads = ${RD:-none}  ($RS)"
[ -n "${RD:-}" ] && awk "BEGIN{exit !($RD>0)}" && pass "1c Ramulator reads > 0" || fail "1c no Ramulator reads"

# 1d: determinism
R2=occarb/step1/out_run2; run $R2 >/dev/null
C2=$(cycles $R2)
[ "${C1:-x}" = "${C2:-y}" ] && pass "1d deterministic ($C1 == $C2)" \
  || echo "WARN 1d cycles differ ($C1 vs $C2): record variance; use multiple runs/averages"

exit $FAILED
