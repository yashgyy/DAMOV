# Paper changes: original .tex → current .tex

Original: `paper1_phase_aware_scheduling.orig.tex` · Current: `paper1_phase_aware_scheduling.tex`
(5-workload intermediate version: `paper1_phase_aware_scheduling.5wl.tex`). Status as of 2026-11-25.

Unchanged: title, author block, keywords, Introduction paragraph 1, the three Ingress/Train/Egress bullets
(incl. the ResNet citation), Fig. 1 except one label, Table I rows IDLE and INGRESS, the Ingress paragraph,
Sec. II-C (Cost), Sec. V (Conclusion), bibliography. Every number now in the paper comes from the DAMOV-SIM runs
recorded in `docs/phasched/PROGRESS_LOG.md`. All `% CHANGED` / `% FILLED` lines are notes — delete before submission.

## 1. Abstract (last sentence)
- Was: "Evaluated in DAMOV-SIM on [N] FL workloads, it reduces round time by [X%] and average read latency by [Y%] over FR-FCFS."
- Now: "Evaluated in DAMOV-SIM on three dense-model FL clients, it reduces round time by 0.5% on average (up to 1.1%)
  and average training read latency by 1.7% (up to 2.8%) over FR-FCFS, and never lengthens a round."

## 2. Introduction, last sentence of paragraph "A conventional controller…"
- Was: "As a result the controller toggles between read and write mode at the wrong moments, pays bus-turnaround
  delays, and interrupts row-hit streaks on streams that are perfectly sequential."
- Now: "As a result a single write-drain policy must serve both the bulk NIC transfers and local training, where
  gradient and weight write-backs interleave with the reads that determine progress: in our measurements training
  issues one bus turnaround every nine requests and its average read latency is 2.7× that of a read-only stream."
- Why: measured NIC-only phases have 1–8 turnarounds and ≈99 % row hits under every scheduler; the measured
  interference is in training (MLP: 788 k turnarounds / 6.9 M requests; read latency 146 vs 55 DRAM cycles).

## 3. Contribution paragraph
- "requires three register writes per round" → "requires four register writes per round".

## 4. Fig. 1
- FL-runtime box label "3 writes/round" → "4 writes/round". Nothing else.

## 5. Sec. II-A Phase Interface
- Added after "…before it starts sending the update": "; a fourth write returns it to IDLE at the end of the round."
  Rest of the paragraph unchanged.

## 6. Table I (presets)
- TRAIN write drain: "lazy: drain when reads idle or W_hi" → "baseline + age bound T_max".
- TRAIN intent: "reads never wait behind writes" → "no write backlog blocks reads".
- EGRESS row-hit cap: "raised" → "unlimited".
- Why: lazy drain was the worst tested variant (+0.3 % round vs FR-FCFS, more turnarounds); baseline drain +
  T_max age bound was best (−1.13 % on MLP, robust for T_max 1000–8000). Baseline FR-FCFS has no cap, so
  "raised" was meaningless.

## 7. Sec. II-B paragraphs
- Train (rewritten). Was: "Train favours reads. Writes of gradients and weights are buffered and drained when the
  read queue is empty or the write queue reaches W_hi. The cap stays at its baseline value to preserve fairness
  among training threads."
  Now: "Train keeps the baseline watermarks, so gradient and weight writes are drained in batches, but bounds their
  age: a write that has waited longer than T_max triggers a drain. This prevents a backlog of write-backs from later
  blocking the reads in a long burst. Leaving write mode as soon as a read arrives performed worse, because it splits
  the drain into many short bursts that each pay a turnaround. The cap stays at its baseline value."
- Egress: "with a raised cap" → "without a row-hit cap".
- Starvation guard. Was: "In every phase a request that has waited longer than T_max is served next. This bounds…"
  Now: "In every FL phase a request that has waited longer than T_max is served next, and the controller switches to
  its queue if needed. This bounds… We use T_max = 2000 DRAM cycles (1.7 µs)."

## 8. Sec. III Methodology (text added; original sentences kept)
Inserted after "…forwards the phase to the Ramulator controller.":
"We enable DAMOV-SIM's memory-clock divider so that DDR4-2400 runs at 1.2 GHz against the 2.4 GHz cores.
Workloads: DAMOV contains no FL client, so we implement three dense-model synthetic FedAvg clients (MLP, LSTM, GRU)
in C++/OpenMP with real forward and backward passes on synthetic local data (256 MB per client); all parameters are
kept in one buffer that the DMA injector transfers. Each run simulates one round with K local minibatches; ingress and
egress times are taken from the DMA injector."

## 9. Table II (configuration)
| Row | Was | Now |
|---|---|---|
| Cores | [4] × 4-wide OOO x86-64 | 4 × 4-wide OOO x86-64, 2.4 GHz |
| Caches | 32 kB L1, 256 kB L2, 8 MB L3 | unchanged |
| Memory | DDR4-2400, [1] channel, [64]-entry queues | DDR4-2400, 1 channel, 1 rank, 32-entry queues |
| Network | [10] GbE, DMA injector | 10 GbE (1.25 GB/s), DMA injector |
| Workloads | ResNet-18, MobileNetV2, 2-layer LSTM | MLP 1024-2048-2048-10 (24 MB); 2-layer LSTM / GRU, hidden 512 (14 / 11 MB) |
| Round | [E] local epochs, batch [B] | one round; K = 2/1/1 minibatches of 32/8/8 samples (MLP/LSTM/GRU) |

## 10. Sec. IV Results paragraph
- Was: "…shortens the round by [X%] on average (up to [Xmax%]). Ingress and egress shrink by [A%] and [B%] because bus
  turnarounds drop by [T%]; average read latency during training falls by [Y%]. BLISS gains [Z%], confirming that
  thread-level fairness does not capture phase behaviour. DRAM energy changes by [E%]."
- Now: "…shortens the round by 0.5% on average (geometric mean) and by up to 1.1% (MLP), and is never slower than
  FR-FCFS. At 10 GbE, ingress and egress are bounded by the link and take the same time under every scheduler; the
  gain comes from training, where gradient and weight write-backs compete with reads: average read latency falls by
  1.7% on average and by up to 2.8% (GRU), and the median read latency of the MLP drops by 20%. Scheduling matters on
  these clients: FCFS lengthens the round by 7.6% on average. The row-hit cap costs 1.0%, and BLISS lengthens the
  round by 1.3%, confirming that thread-level fairness does not capture phase behaviour. DRAM energy, estimated from
  the DRAM command counts and DDR4 datasheet currents, falls by 0.5% on average and by up to 1.2% (MLP), from the
  shorter round and up to 11% fewer row activations."
- Removed [A%], [B%], [T%]: NIC transfer time is identical for all schedulers (DMA timestamps).

## 11. Fig. 2
| Item | Was | Now |
|---|---|---|
| x-axis | ResNet-18, MobileNetV2, LSTM, Geomean (symbolic) | positions 1–4 labelled MLP, LSTM, GRU, **Geomean** (bold) |
| y-axis | 0 – 1.2 | 0.97 – 1.04, ticks every 0.01 |
| FR-FCFS | 0 | 1 / 1 / 1 / 1 |
| FR-FCFS-Cap | 0 | 1.0079 / 1.0120 / 1.0094 / 1.0098 |
| BLISS | 0 | 1.0301 / 1.0042 / 1.0047 / 1.0129 |
| PhaSched | 0 | 0.9887 / 0.9984 / 0.9985 / 0.9952 |
| Separator | — | dashed grey vertical line at x = 3.5 (between GRU and Geomean) |
| Red note | "[RESULTS TO BE INSERTED]" | removed |
| Caption | "…(lower is better)." | "…(lower is better; y-axis starts at 0.97). Geomean: geometric mean over the three clients (right of the dashed line)." |

## Open items
- Compile and check Fig. 2 rendering (not compiled here — no LaTeX installed).
- Runs used EGRESS cap 64; text says "unlimited" (no effect at 10 GbE, link-bound). Re-run PhaSched_final with
  `phasched_egress_cap = 1000000`, or change the text.
- Reviewer risk: phase-agnostic control (FR-FCFS + age bound without phases) not yet run.
- Energy uses typical (not exact-datasheet) DDR4 IDD values (`phasched/scripts/energy.py`).
