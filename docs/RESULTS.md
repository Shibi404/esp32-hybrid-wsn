# Performance Results

**23CSE341 WSN — Lab Evaluation 2** · deliverable: *Performance results*

> Fill the tables from one continuous run. `tools/plot_metrics.py` writes
> `results/run_*/summary.md` with these numbers already formatted — paste them
> here, and drop the generated PNGs into `docs/images/`.

---

## Run conditions

| Item | Value |
|---|---|
| Date | ____________ |
| Room / layout | ____________ |
| Anchor coordinates | N2 (0, 0) · N3 (4.0, 0) · N4 (0, 3.0) m |
| N1 ground truth | (____, ____) m |
| Run length | ______ s |
| Superframe | 2000 ms, 8 × 250 ms slots |
| Channel | 1 |
| RSSI @ 1 m (calibrated) | ______ dBm |
| Path-loss exponent *n* | ______ |
| MQ heater voltage | 3.3 V (see §6) |

---

## 1. Summary — the seven metrics

| # | Metric | Measured | How it was obtained |
|---|---|---|---|
| 1 | Packet Delivery Ratio | ______ % | sink: received ÷ sequence span, per source |
| 2 | Packet loss | ______ packets (______ %) | sink: gaps in the sequence numbers |
| 3 | End-to-end delay | ______ ms mean, ______ ms max | sink: netTime − txTimeMs, beacon-synced clock |
| 4 | Throughput | ______ bit/s | sink: payload bits ÷ elapsed time |
| 5 | Retransmissions | ______ | MAC retry counters, reported in each data frame |
| 6 | Localization error | ______ m mean, ______ m RMSE | N1: estimate vs ground truth |
| 7 | Data reduction | ______ % bytes, ______ % packets | N3: 1 − aggBytes ÷ rawBytes |

*(The brief requires four. All seven are measured.)*

---

## 2. Per-source delivery

| Source | Received | Expected | Lost | PDR (%) | Mean delay (ms) | Hops | Path |
|---|---|---|---|---|---|---|---|
| N1 | | | | | | | `1→3→4` |
| N2 | | | | | | | `2→3→4` |
| N3 (aggregate) | | | | | | | `3→4` |

**Interpretation.** *Which source performed worst, and why? A node further
from the cluster head, or one that spent time on the alternate route, should
show both a lower PDR and a higher mean delay — say so and point at the
numbers.*

---

## 3. MAC behaviour

| Quantity | Value | Note |
|---|---|---|
| Transmissions | | |
| Acknowledged | | |
| ACK success rate | ______ % | |
| Mean ACK round trip | ______ ms | one-hop MAC latency |
| Retransmissions | | |
| ACK timeouts | | |
| Dropped at the retry limit | | |
| CCA busy deferrals | | medium found busy |
| **Backoff freezes** | | **contention actually resolved** |
| Mean backoff | ______ slots | expected ≈ (CW−1)/2 = 3.5 at CW = 8 |
| Max contention window reached | | 8 → 16 → 32 → 64 under load |
| Forced transmissions | | medium busy > 50 ms |

**Collision experiment.** Record the one you demonstrated:

| | N1 | N2 |
|---|---|---|
| Backoff drawn (slots) | | |
| Outcome | transmitted first / froze at ___ | |
| Delivered? | | |
| Attempts needed | | |

**Interpretation.** *Compare the measured mean backoff with the theoretical
(CW−1)/2. Then state what the freeze count tells you: every freeze is a
collision that did not happen.*

---

## 4. Route failure and recovery

| Event | Value |
|---|---|
| Failure injected at | t = ______ s (`fail 3` / physically unplugged) |
| Detection mechanism | ______ consecutive ACK losses / 7 s silence |
| Detection latency | ______ ms |
| Alternate route chosen | `1→2→4` |
| **Failover latency** (detect → alternate) | ______ ms |
| Packets lost during failover | ______ |
| PDR during the failure window | ______ % |
| Recovery detected at | t = ______ s |
| Route restored to | `1→3→4` |
| Cost before / during / after | ______ / ______ / ______ |

**Interpretation.** *Failover latency is dominated by detection: three ACK
timeouts at 60 ms each. Note that lowering the timeout would speed recovery
but cause false positives on a merely slow link — state where you think the
trade sits.*

---

## 5. Aggregation

| Quantity | Value |
|---|---|
| Aggregation events | |
| Raw packets folded in | |
| Bytes without aggregation | |
| Bytes with aggregation | |
| **Byte reduction** | ______ % |
| **Packet reduction** | ______ % |

Per-superframe arithmetic:

```
  3 x DATA = 3 x (21 header + 24 payload + 2 CRC) = 3 x 47 = 141 B, 3 packets
  1 x AGG  = 1 x (21 header + 52 payload + 2 CRC) = 1 x 75 =  75 B, 1 packet

  byte reduction   = (1 -  75/141) x 100 = 46.8 %
  packet reduction = (1 -   1/3  ) x 100 = 66.7 %
```

**Interpretation.** *Packet reduction is the more meaningful figure: each
transmission costs a backoff, preamble, ACK and receive window regardless of
payload size, so per-packet overhead dominates energy in a WSN. Byte reduction
is the conservative number. Note also what aggregation preserves — min and max
alongside the mean, and the OR of the alarm bits — and why discarding them
would make the aggregate useless for event detection.*

---

## 6. Localization

| Round | Est. (x, y) m | True (x, y) m | Error (m) |
|---|---|---|---|
| 1 | | | |
| 2 | | | |
| 3 | | | |
| 4 | | | |
| 5 | | | |
| **Mean** | | | |
| **RMSE** | | | |

Per-anchor ranging error at one position:

| Anchor | Median RSSI (dBm) | Estimated distance (m) | True distance (m) | Error (m) |
|---|---|---|---|---|
| N2 | | | | |
| N3 | | | | |
| N4 | | | | |

Optionally repeat at three positions to show how error varies with geometry:

| Position | True (x, y) | Mean error (m) |
|---|---|---|
| A (centre) | | |
| B (near an anchor) | | |
| C (near a corner) | | |

**Interpretation.** *Expect roughly 0.3–1.0 m indoors. Attribute the error:
RSSI ranging noise first, then the single global path-loss exponent, then
geometric dilution from only three anchors in a small triangle. If error is
noticeably worse near a corner, that is dilution of precision — say so.*

---

## 7. Energy / duty cycle

| Node | Slots owned | Slots used | Unused | Sleep slots | Radio duty cycle |
|---|---|---|---|---|---|
| N1 | | | | | ______ % |
| N2 | | | | | ______ % |
| N3 | | | | | ______ % |
| N4 | — | — | — | 0 | 100 % (gateway) |

**Interpretation.** *A leaf node that is awake under half the time is spending
under half the radio energy of an always-on node. State the trade explicitly:
that saving is bought with up to one superframe of latency.*

---

## 8. Figures

| Figure | File |
|---|---|
| PDR and cumulative loss over time | `results/run_*/pdr_over_time.png` |
| End-to-end delay distribution | `results/run_*/delay_distribution.png` |
| Throughput over the run | `results/run_*/throughput.png` |
| Backoff distribution and CW growth | `results/run_*/mac_activity.png` |
| Localization error and scatter | `results/run_*/localization.png` |
| Bytes with vs without aggregation | `results/run_*/aggregation.png` |

---

## 9. Overall interpretation

*Three or four paragraphs. Suggested spine:*

1. **Reliability.** PDR and what the losses were — mostly during failover, or
   spread evenly? An even spread points at RF conditions; a burst points at
   the route change.
2. **Latency.** Decompose it: one-hop MAC latency (the ACK RTT) versus the
   TDMA waiting time. Make clear the delay is a scheduling choice, not a radio
   limitation.
3. **Efficiency.** Aggregation reduction plus duty cycle, and the single
   sentence tying them together: both trade latency for energy, which is the
   correct trade for a sensor network.
4. **Localization.** Error magnitude and its dominant cause, with one concrete
   thing that would improve it.
