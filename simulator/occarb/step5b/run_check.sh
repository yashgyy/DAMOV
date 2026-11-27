#!/bin/bash
# OCCARB Step 5b checks: NIC DMA injector integrated into ZSim<->Ramulator.
# Single-core STREAM (bit-deterministic within a build). Run from simulator/ AFTER bash occarb/rebuild.sh:
#   bash occarb/step5b/run_check.sh
set -u
source "$(dirname "$0")/../lib.sh"; cd "$SIM"
OUT=occarb/step5b/out; rm -rf $OUT; mkdir -p $OUT
CMD="$WL/STREAM/stream_copy 1"; INSTRS=20000000L
pct(){ awk "BEGIN{printf \"%.3f\", ($1-$2)/$2*100}"; }
absok(){ awk "BEGIN{d=($1-$2)/$2; if(d<0)d=-d; exit !(d<=$3)}"; }   # |a-b|/b <= tol

# b0: earlier steps still OK
bash occarb/step4/run_check.sh > $OUT/step4.txt 2>&1 && pass "b0 Step 4 NIC unit tests" || fail "b0 Step 4 unit tests (see $OUT/step4.txt)"
REF1=26587526
bash occarb/step1/run_check.sh > $OUT/step1.txt 2>&1
C1=$(grep -m1 'core cycles' $OUT/step1.txt | awk '{print $4}')
grep -q "PASS 1d" $OUT/step1.txt && absok $C1 $REF1 0.005 && pass "b0 Step 1 within 0.5% ($C1 vs $REF1)" || fail "b0 Step 1: $C1 vs $REF1"
grep -q "\[NIC\] disabled" occarb/step1/out_run1/zsim.log && pass "b0 configs without nic block -> NIC disabled" || fail "b0 '[NIC] disabled' missing in Step 1 log"

# mk NAME [key value]... -> config from template (1 core), with nic keys set
mk(){ local d=$OUT/$1; shift; mkdir -p $d
      gen_cfg $d/run.cfg 1 r "$CMD" $INSTRS ramulator-configs/DDR4-occarb.cfg
      while [ $# -gt 0 ]; do nic_set $d/run.cfg $1 $2; shift 2; done
      rc=$(run_dir $d); [ "$rc" = 0 ] && [ -f $d/r.zsim.out ] || fail "run $(basename $d) failed (see $d/zsim.log)"; }
T(){ max_cycles $OUT/$1 r; }
Z(){ zstat $OUT/$1 r $2; }
RS(){ rstat $OUT/$1 r $2; }

mk base   enable false
mk rho0   enable true rho 0.0
mk rx10   enable true tx false linkGbps 10.0
mk rx10b  enable true tx false linkGbps 10.0
mk tx10   enable true rx false linkGbps 10.0
mk starve enable true tx false linkGbps 25.0 bufKB 16 maxInflight 1
mk both25 enable true linkGbps 25.0
T0=$(T base)

# b1: nic block present but disabled
grep -q "\[NIC\] disabled" $OUT/base/zsim.log && [ -z "$(Z base nicCycles)" ] && pass "b1 enable=false -> NIC disabled, no NIC stats (T0=$T0)" || fail "b1 disabled NIC"

# b2: enabled with rho=0 -> NIC ticks but injects nothing -> bit-identical timing
grep -q "\[NIC\] enabled" $OUT/rho0/zsim.log && pass "b2 enable=true -> '[NIC] enabled' logged" || fail "b2 '[NIC] enabled' missing"
[ "$(Z rho0 nicRxDmaWrites)" = 0 ] && [ "$(Z rho0 nicTxDmaReads)" = 0 ] && pass "b2 rho=0 -> zero DMA requests" || fail "b2 rho=0 issued DMA"
[ "$(T rho0)" = "$T0" ] && pass "b2 rho=0 -> cycles identical to NIC disabled ($T0)" || fail "b2 rho=0 cycles $(T rho0) vs $T0"

# b3: RX only, 10 GbE, rho=1, S=1 MB -> link-rate delivery, zero loss, DMA writes visible in Ramulator
NC=$(Z rx10 nicCycles); NA=$(Z rx10 nicActiveCycles); ACC=$(Z rx10 nicRxBytesAccepted); WR=$(Z rx10 nicRxBytesWritten)
DW=$(Z rx10 nicRxDmaWrites); DROP=$(Z rx10 nicRxPktsDropped); T3=$(T rx10)
EXP=$(awk "BEGIN{printf \"%.0f\", $NA*10e9/8/2.4e9}")
echo "     rx10: cycles=$T3 nicCycles=$NC accepted=$ACC (expected $EXP) written=$WR dmaWrites=$DW dropped=$DROP meanLat=$(awk "BEGIN{printf \"%.1f\", $(Z rx10 nicRxDmaLat)/$DW}") cyc"
absok $NC $T3 0.01 && pass "b3 NIC time base = core time (nicCycles $NC ~ cycles $T3)" || fail "b3 nicCycles $NC vs cycles $T3"
absok $ACC $EXP 0.01 && pass "b3 10GbE delivers 1.25 GB/s ($ACC B vs $EXP B, $(pct $ACC $EXP)%)" || fail "b3 accepted $ACC vs expected $EXP"
[ "$DROP" = 0 ] && pass "b3 zero packet loss with S=1MB" || fail "b3 dropped $DROP packets"
awk "BEGIN{exit !($WR >= $ACC - 1048576)}" && pass "b3 accepted data reaches DRAM (written >= accepted - S)" || fail "b3 written $WR vs accepted $ACC"
DWR=$(( $(RS rx10 write_requests) - $(RS base write_requests) ))
absok $DWR $DW 0.02 && pass "b3 Ramulator write_requests grew by the DMA writes ($DWR vs $DW)" || fail "b3 write_requests delta $DWR vs DMA writes $DW"
[ "$T3" -gt "$T0" ] && pass "b3 RX DMA slows the core (S = $(awk "BEGIN{printf \"%.4f\", $T3/$T0}"))" || fail "b3 no slowdown ($T3 vs $T0)"
[ "$(T rx10b)" = "$T3" ] && pass "b3 deterministic with NIC on ($T3)" || fail "b3 rerun differs: $(T rx10b) vs $T3"

# b4: TX only, 10 GbE -> link-rate upload, few underruns, DMA reads visible in Ramulator
NA=$(Z tx10 nicActiveCycles); SENT=$(Z tx10 nicTxBytesSent); PK=$(Z tx10 nicTxPktsSent); UR=$(Z tx10 nicTxUnderruns); DR=$(Z tx10 nicTxDmaReads)
EXP=$(awk "BEGIN{printf \"%.0f\", $NA*10e9/8/2.4e9}")
echo "     tx10: cycles=$(T tx10) sent=$SENT (expected $EXP) pkts=$PK underruns=$UR dmaReads=$DR meanLat=$(awk "BEGIN{printf \"%.1f\", $(Z tx10 nicTxDmaLat)/$DR}") cyc"
absok $SENT $EXP 0.02 && pass "b4 TX sends at link rate ($(pct $SENT $EXP)%)" || fail "b4 sent $SENT vs expected $EXP"
awk "BEGIN{exit !($UR <= 0.01*($PK+$UR))}" && pass "b4 underruns <= 1% of packet slots ($UR)" || fail "b4 underruns $UR of $(($PK+$UR))"
DRD=$(( $(RS tx10 read_requests) - $(RS base read_requests) ))
absok $DRD $DR 0.02 && pass "b4 Ramulator read_requests grew by the DMA reads ($DRD vs $DR)" || fail "b4 read_requests delta $DRD vs DMA reads $DR"

# b5: starved DMA engine (1 outstanding write, 16 KB buffer, 25 GbE) -> packet loss
L=$(Z starve nicRxPktsDropped); A=$(Z starve nicRxPktsArrived)
echo "     starve: arrived=$A dropped=$L loss=$(awk "BEGIN{printf \"%.3f\", $L/$A}") meanLat=$(awk "BEGIN{printf \"%.1f\", $(Z starve nicRxDmaLat)/$(Z starve nicRxDmaWrites)}") cyc"
[ "$L" -gt 0 ] && pass "b5 drain slower than link -> packet loss" || fail "b5 no loss with a starved DMA engine"

# b6: RX+TX at 25 GbE slows the core more than RX at 10 GbE
S3=$(awk "BEGIN{printf \"%.4f\", $T3/$T0}"); S6=$(awk "BEGIN{printf \"%.4f\", $(T both25)/$T0}")
awk "BEGIN{exit !($S6 > $S3 && $S3 > 1)}" && pass "b6 slowdown ordering: S(RX+TX 25G)=$S6 > S(RX 10G)=$S3 > 1" || fail "b6 S6=$S6 S3=$S3"
echo "     both25: loss=$(awk "BEGIN{printf \"%.4f\", $(Z both25 nicRxPktsDropped)/$(Z both25 nicRxPktsArrived)}") underruns=$(Z both25 nicTxUnderruns)"

# b7: occupancy-level time accounting
SUM=$(( $(Z both25 nicLevel0Cycles) + $(Z both25 nicLevel1Cycles) + $(Z both25 nicLevel2Cycles) + $(Z both25 nicLevel3Cycles) ))
[ "$SUM" = "$(Z both25 nicCycles)" ] && pass "b7 level cycles sum to NIC cycles ($SUM)" || fail "b7 level sum $SUM vs $(Z both25 nicCycles)"
echo "     both25 level residency: L0=$(Z both25 nicLevel0Cycles) L1=$(Z both25 nicLevel1Cycles) L2=$(Z both25 nicLevel2Cycles) L3=$(Z both25 nicLevel3Cycles)"
exit $FAILED
