#!/bin/bash
# usage: flsum.sh <prefix> <suffix>   e.g.  flsum.sh fl_dnn _10g
# One line per scheduler: TRAIN cycles, TRAIN core-read latency (avg/p50/p99, DRAM clk), TRAIN turnarounds,
# NIC transfer cycles (from dma.csv) and round = dmaW + TRAIN + dmaR (CPU cycles).
pre=$1; suf=$2
printf "%-11s %12s %8s %6s %6s %9s %11s %11s %12s\n" sched train_cyc rd_avg p50 p99 turn dmaW_cyc dmaR_cyc round_cyc
for s in FCFS FRFCFS FRFCFS_Cap BLISS PhaSched; do
  f=${pre}_${s}${suf}
  [ -f "$f.phases.csv" ] || continue
  awk -F, -v s=$s -v dma="$f.dma.csv" '
    $1==2 { c+=$3-$2; r+=$5; l+=$6; t+=$11; p50=$7; p99=$8 }
    END {
      while ((getline line < dma) > 0) { split(line, a, ","); if (a[1]=="W") w=a[4]-a[3]; if (a[1]=="R") rr=a[4]-a[3] }
      printf "%-11s %12d %8.1f %6d %6d %9d %11d %11d %12d\n", s, c, (r?l/r:0), p50, p99, t, w, rr, w+c+rr }' "$f.phases.csv"
done
