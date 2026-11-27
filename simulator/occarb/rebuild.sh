#!/bin/bash
# Rebuild Ramulator (clean: its Makefile has no header dependencies) and ZSim.
# Run from simulator/:  bash occarb/rebuild.sh
set -e
SIM=$(cd "$(dirname "$0")/.." && pwd); cd "$SIM"
( cd ramulator && make clean >/dev/null && make -j"$(nproc)" libramulator.so )
export PINPATH="$SIM/pin" RAMULATORPATH="$SIM/ramulator" LIBCONFIGPATH="$SIM/libconfig"
scons -j"$(nproc)"
echo "REBUILD OK"
