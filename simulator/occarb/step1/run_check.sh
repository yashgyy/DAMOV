#!/bin/bash
# OCCARB Step 1 checks. Run from simulator/:  bash occarb/step1/run_check.sh
set -u
SIM=$(cd "$(dirname "$0")/../.." && pwd); cd "$SIM"
WL=$(cd "$SIM/../workloads" && pwd)
pass(){ echo "PASS $1"; }; fail(){ echo "FAIL $1"; FAILED=1; }; FAILED=0

# 1a: build artifacts + workload binary
[ -x build/opt/zsim ] && pass "1a zsim binary" || fail "1a build/opt/zsim missing"
[ -f ramulator/libramulator.so ] && pass "1a libramulator.so" || fail "1a libramulator.so missing"
[ -x "$WL/STREAM/stream_copy" ] && pass "1a stream_copy binary" || fail "1a $WL/STREAM/stream_copy missing (run compile.py in workloads/STREAM)"

cycles(){ grep -m1 -E '^\s+cycles:' "$1/smoke.zsim.out" | awk '{print $2}'; }
run(){ rm -rf "$1"; mkdir -p "$1"
       sed "s#WORKLOADS_ROOT#$WL#" "$SIM/occarb/step1/smoke.cfg" > "$1/smoke.cfg"
       (cd "$1" && ln -sf "$SIM/ramulator-configs" . && \
        "$SIM/build/opt/zsim" "$SIM/$1/smoke.cfg" > zsim.log 2>&1); echo $?; }

# 1b: run completes and produces stats
R1=occarb/step1/out_run1
rc=$(run $R1)
grep -q "Injector exited" $R1/zsim.log && fail "1b Pin injector crashed (see $R1/zsim.log)"
[ "$rc" = 0 ] && [ -f $R1/smoke.zsim.out ] && pass "1b smoke run (rc=0, smoke.zsim.out)" || fail "1b rc=$rc, see $R1/zsim.log"

# 1c: non-zero activity in core and in Ramulator
C1=$(cycles $R1); echo "     core cycles = ${C1:-none}"
[ -n "${C1:-}" ] && [ "$C1" -gt 0 ] && pass "1c core cycles > 0" || fail "1c no core cycles"
RS=$(ls $R1/*.ramulator.stats 2>/dev/null | head -1)
rstat(){ [ -n "$RS" ] && grep -E "(^|[[:space:].])$1[[:space:]]" "$RS" | head -1 | awk '{print $2}'; }
RD=$(rstat read_requests); WR=$(rstat write_requests)
echo "     ramulator read_requests = ${RD:-none}, write_requests = ${WR:-none}  ($RS)"
[ -n "${RD:-}" ] && awk "BEGIN{exit !($RD>0)}" && pass "1c Ramulator read_requests > 0" \
  || { fail "1c no Ramulator reads"; echo "     first lines of $RS:"; head -15 "$RS"; }

# 1d: determinism
R2=occarb/step1/out_run2; run $R2 >/dev/null
C2=$(cycles $R2)
[ "${C1:-x}" = "${C2:-y}" ] && pass "1d deterministic ($C1 == $C2)" \
  || echo "WARN 1d cycles differ ($C1 vs $C2): record variance; use multiple runs/averages"

exit $FAILED
