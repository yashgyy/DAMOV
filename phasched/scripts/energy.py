#!/usr/bin/env python3
"""DRAM energy from per-phase command counts (phases.csv) and round time.
Usage: energy.py <prefix> <suffix> <schedA> <schedB> ...   e.g. energy.py fl_dnn _10g FRFCFS PhaSched_final
Model (per x8 chip, 8 chips/rank), DDR4-2400 timing tCK=0.833 ns, tRAS=39, tRC=55 clk, BL8 = 4 clk, tRFC(4Gb)=260 ns.
IDD values are typical 4Gb x8 DDR4-2400 datasheet currents -- replace with the exact datasheet values if needed:
  E_ACT = VDD*(IDD0*tRC - IDD3N*tRAS - IDD2N*(tRC-tRAS));  E_RD/WR = VDD*(IDD4R/W - IDD3N)*tBL
  E_REF = VDD*(IDD5B - IDD3N)*tRFC;  background = VDD*IDD3N*T_round
"""
import sys
VDD = 1.2; tCK = 0.833e-9
IDD0, IDD2N, IDD3N, IDD4R, IDD4W, IDD5B = 50e-3, 34e-3, 44e-3, 140e-3, 130e-3, 205e-3
tRAS, tRC, tBL, tRFC, CHIPS = 39 * tCK, 55 * tCK, 4 * tCK, 260e-9, 8
E_ACT = VDD * (IDD0 * tRC - (IDD3N * tRAS + IDD2N * (tRC - tRAS)))
E_RD = VDD * (IDD4R - IDD3N) * tBL; E_WR = VDD * (IDD4W - IDD3N) * tBL; E_REF = VDD * (IDD5B - IDD3N) * tRFC

def run(f):
    act = rd = wr = ref = train = 0
    for i, l in enumerate(open(f + ".phases.csv")):
        if i == 0: continue
        c = l.strip().split(",")
        act += int(c[11]); rd += int(c[13]); wr += int(c[14]); ref += int(c[15])
        if c[0] == "2": train += int(c[2]) - int(c[1])
    w = r = 0
    for l in open(f + ".dma.csv"):
        c = l.strip().split(",")
        if c[0] == "W": w = int(c[3]) - int(c[2])
        if c[0] == "R": r = int(c[3]) - int(c[2])
    T = (w + train + r) / 2.4e9
    dyn = act * E_ACT + rd * E_RD + wr * E_WR + ref * E_REF
    return CHIPS * (dyn + VDD * IDD3N * T)

pre, suf, scheds = sys.argv[1], sys.argv[2], sys.argv[3:]
base = None
for s in scheds:
    e = run(f"{pre}_{s}{suf}")
    base = base or e
    print(f"{s:20s} {e*1e3:9.2f} mJ  norm {e/base:.4f}")
