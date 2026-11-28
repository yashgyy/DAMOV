#!/bin/bash
# usage: flmean.sh <workload>   (dnn | lstm | gru)
# Averages all finished repeats (<name>, <name>_rep, _r2, _r3) per scheduler.
# Prints: runs, mean/min/max round (CPU cycles), mean TRAIN read latency, mean round normalised to FR-FCFS mean.
m=$1
round() { awk -F, -v dma="$1.dma.csv" '$1==2{c+=$3-$2} END{while((getline l<dma)>0){split(l,a,",");if(a[1]=="W")w=a[4]-a[3];if(a[1]=="R")r=a[4]-a[3]} print w+c+r}' "$1.phases.csv"; }
lat()   { awk -F, '$1==2{r+=$5;l+=$6} END{print (r?l/r:0)}' "$1.phases.csv"; }
base=""
printf "%-14s %3s %14s %14s %14s %8s %8s\n" sched runs mean_round min max rd_avg norm
for s in FRFCFS FRFCFS_Cap BLISS PhaSched_v2; do
  vals=""; lats=""
  for suf in "" _rep _r2 _r3; do
    f=fl_${m}_${s}${suf}_10g
    [ -f "$f.phases.csv" ] && grep -q '^R' "$f.dma.csv" 2>/dev/null || continue
    vals="$vals $(round $f)"; lats="$lats $(lat $f)"
  done
  [ -z "$vals" ] && continue
  echo "$s $vals | $lats" | awk -v base="$base" '{n=0;s=0;mn=1e30;mx=0;i=2
    while($i!="|"){v=$i;s+=v;n++;if(v<mn)mn=v;if(v>mx)mx=v;i++}
    i++;ls=0;ln=0;for(;i<=NF;i++){ls+=$i;ln++}
    mean=s/n; printf "%-14s %3d %14.0f %14.0f %14.0f %8.1f %8.4f\n",$1,n,mean,mn,mx,ls/ln,(base?mean/base:1)}'
  [ -z "$base" ] && base=$(echo "$vals" | awk '{s=0;for(i=1;i<=NF;i++)s+=$i;print s/NF}')
done
exit 0
