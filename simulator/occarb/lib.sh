# Shared helpers for OCCARB check scripts. Source from a script run in simulator/.
SIM=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WL=$(cd "$SIM/../workloads" && pwd)
FAILED=0
pass(){ echo "PASS $1"; }
fail(){ echo "FAIL $1"; FAILED=1; }

# gen_cfg OUT_CFG CORES STATS CMD MAX_INSTRS RAMCFG  -> writes a cfg from template_occarb.cfg
gen_cfg(){
  sed -e "s#NUMBER_CORES#$2#g" -e "s#STATS_PATH#$3#g" -e "s#COMMAND_STRING#$4#g" \
      -e "s#MAX_INSTRS#$5#g" -e "s#RAMULATOR_CONFIG#$6#g" \
      "$SIM/templates/template_occarb.cfg" > "$1"
}

# run_dir DIR STATS  -> runs DIR/run.cfg inside DIR, echoes zsim rc
#   DIR must already contain run.cfg; ramulator-configs is symlinked unless DIR has its own.
run_dir(){
  ( cd "$1" && { [ -e ramulator-configs ] || ln -s "$SIM/ramulator-configs" .; } && \
    "$SIM/build/opt/zsim" "$(pwd)/run.cfg" > zsim.log 2>&1 ); echo $?
}

# max_cycles DIR STATS -> max per-core cycles from <STATS>.zsim.out (= execution time)
#   (grep does the matching: mawk on this system has no [[:space:]] support)
max_cycles(){ grep -E '^\s+cycles:' "$1/$2.zsim.out" 2>/dev/null | awk '{if($2>m)m=$2}END{print m+0}'; }
# ncores DIR STATS -> number of simulated cores in zsim.out
ncores(){ grep -cE '^[[:space:]]+core-[0-9]+:' "$1/$2.zsim.out" 2>/dev/null; }
# rstat DIR STATS NAME -> Ramulator stat value
rstat(){ grep -E "(^|[[:space:].])$3[[:space:]]" "$1/$2.ramulator.stats" 2>/dev/null | head -1 | awk '{print $2}'; }
# zstat DIR STATS NAME -> first value of a zsim.out stat (e.g. nicRxPktsDropped)
zstat(){ grep -E "^\s+$3:" "$1/$2.zsim.out" 2>/dev/null | head -1 | awk '{print $2}'; }
# nic_set CFG KEY VALUE -> set a key inside the cfg (keys in the nic block are unique in the template)
nic_set(){ sed -i -E "s/^(\s*$2 = )[^;]*;/\1$3;/" "$1"; }
# ram_policy DIR POLICY -> give DIR its own ramulator-configs/DDR4-occarb.cfg with "src_policy = POLICY"
ram_policy(){ mkdir -p "$1/ramulator-configs"
  grep -v "src_policy" "$SIM/ramulator-configs/DDR4-occarb.cfg" > "$1/ramulator-configs/DDR4-occarb.cfg"
  echo " src_policy = $2" >> "$1/ramulator-configs/DDR4-occarb.cfg"; }
# ram_set DIR KEY VALUE -> add/replace a key in DIR/ramulator-configs/DDR4-occarb.cfg (call ram_policy first)
ram_set(){ local f="$1/ramulator-configs/DDR4-occarb.cfg"; grep -v "^ $2 = " "$f" > "$f.tmp"; mv "$f.tmp" "$f"; echo " $2 = $3" >> "$f"; }
