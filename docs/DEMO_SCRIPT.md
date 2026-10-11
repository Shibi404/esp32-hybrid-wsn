# Demonstration Script

**23CSE341 WSN — Lab Evaluation 2**

The guidelines specify an exact demonstration sequence. This script walks it
end to end and tells you what to say at each step. Total running time about
**12 minutes**, plus viva.

---

## Before the examiner arrives (20 minutes earlier)

| # | Action | Why |
|---|---|---|
| 1 | Power all four nodes and leave them running | MQ sensors need 120 s warm-up; longer is better |
| 2 | Place the nodes at their tape-measured positions | Localization error is scored against `wsn_config.cpp` |
| 3 | Join Wi-Fi `WSN_SINK` / `wsnlab2024`, open `http://192.168.4.1` | Confirm the dashboard is live |
| 4 | Open serial monitors for **N1** and **N3** at 115200 | These two show the most during the demo |
| 5 | On N2 and N3 type `calib` (clean air, away from breath/lighter) | Gives trustworthy `Rs/R0` ratios |
| 6 | On N1 type `calib1m 3`, holding N1 exactly 1 m from N3 | Calibrates the ranging model to this room |
| 7 | On N1 type `settrue 2.0 1.5` (or wherever you put it) | Sets the ground truth the error is measured against |
| 8 | Watch one full cycle and confirm PDR > 90 % | A bad supply shows up here as fake "packet loss" |
| 9 | Start the logger: `python tools/serial_logger.py --port COM<n>` on the sink | Captures the run you are about to demonstrate |

> If a node shows `Brownout detector was triggered`, move it to a better 5 V
> supply before you start. Every metric you collect afterwards depends on it.

---

## The demonstration

### Step 1 · BLE discovery — *"the network finds itself"*

**Show:** N1's serial console during a BLE phase.

```
[  12.340][N1][BLE  ][I] Scanning for 4 s ...
[  16.410][N1][BLE  ][I] N2: RSSI med=-61 dBm (n=11, ewma=-60.8)  d_est=1.86 m  ...
[  16.412][N1][BLE  ][I] N3: RSSI med=-68 dBm (n=9,  ewma=-67.4)  d_est=3.21 m  ...
[  16.414][N1][BLE  ][I] N4: RSSI med=-64 dBm (n=12, ewma=-64.1)  d_est=2.37 m  ...
```

**Say:** "All four nodes advertise as `WSN-Nx` and scan for each other. N1 is
the mobile node; N2, N3 and N4 are anchors at surveyed positions."

### Step 2 · RSSI measurement — *"and it is filtered, not raw"*

**Say:** "Each neighbour contributes 9–12 RSSI samples per round. We take the
**median**, not the mean — BLE on a crowded 2.4 GHz band throws 15–20 dB
multipath outliers that drag a mean but not a median — then smooth across
rounds with an EWMA so the position does not jitter."

### Step 3 · Localization

```
[  16.418][N1][BLE  ][I] Nearest node: N2 at -61 dBm (~1.86 m)  [proximity result]
[  16.425][N1][BLE  ][I] LOCALIZATION: estimated (2.31, 1.22) m | true (2.00, 1.50) m
                         | error 0.42 m | mean 0.51 m | RMSE 0.55 m | 3 anchors, round 4
```

**Show:** the dashboard's **BLE localization** panel — the map draws the three
anchors, the true position as a green ring, the estimate as an amber dot, and
a dashed line between them whose length *is* the error.

**Say:** "Distance comes from the log-distance path-loss model,
`d = 10^((RSSI₁ₘ − RSSI)/10n)`, with `RSSI₁ₘ` calibrated in this room. Three
distances give three circles; we linearise by subtracting the reference
anchor's equation and solve the 2×2 normal equations in the least-squares
sense. Error is the Euclidean distance to the tape-measured truth."

> **If asked why not more anchors:** with four nodes, three is all we have.
> The solver accepts more and becomes overdetermined, which averages out
> ranging noise.

### Step 4 · Wi-Fi initialisation

```
[  18.500][N1][BLE  ][I] BLE down, controller memory released (free heap 184320 B)
========================= WI-FI NETWORK OPERATION =========================
```

**Say:** "The ESP32 has one 2.4 GHz radio. Running BLE and ESP-NOW together
starves the scan window and biases RSSI — the very thing we just measured — so
we time-multiplex them, which is also the sequence the brief asks for.
`BLEDevice::deinit(true)` returns about 60 kB, which is what lets the sink run
BLE, a SoftAP and a web server on one chip."

### Step 5 · Dynamic schedule assignment

**Show:** N3's console at a beacon, and the dashboard's slot map.

```
  --- SCHEDULE v1 (initial schedule) --------------------------------
   slot   t(ms)        kind     owner   this node
    0       0-250      BEACON   N3      TRANSMIT
    1     250-500      DATA     N1      listen
    2     500-750      DATA     N2      listen
    3     750-1000     SLEEP    -       SLEEP
    4    1000-1250     AGG      N3      TRANSMIT
    5    1250-1500     CAP      ALL     TRANSMIT
    6    1500-1750     STATUS   N4      listen
    7    1750-2000     SLEEP    -       SLEEP
```

**Say:** "N3 is the scheduler and the time master. Its beacon carries the slot
map, a version number and the network time everyone synchronises to — that
shared clock is also what makes the sink's end-to-end delay measurement valid
across different boards."

### Step 6 · Channel sensing and random backoff

**Show:** any node's console.

```
[  22.104][N2][MAC  ][I] CCA idle for DIFS -> backoff = 5 slots (CW=8, slot=500 us)
[  22.110][N2][MAC  ][I] TX DATA    seq=37  UNICAST N2->N3 (final N4, hop 0, 47 B, path 2)
[  22.141][N2][MAC  ][I] ACK  from N3 for seq=37   (31 ms, 1 attempt) - delivered
```

**Say:** "Carrier sense is two mechanisms OR-ed: physical CCA from a
promiscuous-mode sniffer that timestamps every 802.11 frame above −75 dBm, and
virtual carrier sense from a NAV that every one of our frames declares in its
header. Then DIFS, then a random backoff."

### Step 7 · Scheduled transmission and unicast with ACK

Point at the `UNICAST N2->N3 (final N4 …)` line.

**Say:** "`dst` is the next hop, `finalDst` is the destination — separating them
is what makes multi-hop work. Sensor data, next-hop traffic, aggregates and
ACKs are unicast; schedule, route control and status are broadcast."

### Step 8 · **Collision demonstration** (required)

Type on any console, or press **Contention test** on the dashboard:

```
collide
```

**Show:** N1 and N2 side by side.

```
N1: [MAC][I] CCA idle for DIFS -> backoff = 2 slots (CW=8, slot=500 us)
N2: [MAC][I] CCA idle for DIFS -> backoff = 6 slots (CW=8, slot=500 us)
N1: [MAC][I] TX DATA seq=41 UNICAST N1->N3 ...
N2: [MAC][I] Medium went BUSY - backoff FROZEN at 3 slots (will resume, not restart)
N1: [MAC][I] ACK from N3 for seq=41 (29 ms, 1 attempt) - delivered
N2: [MAC][I] TX DATA seq=38 UNICAST N2->N3 ...
N2: [MAC][I] ACK from N3 for seq=38 (33 ms, 1 attempt) - delivered
```

**Say:** "Both nodes were granted the same slot, so both had a frame ready at
the same instant. They drew different backoffs — 2 and 6. N1 won. The
important line is N2's: the backoff **freezes** at 3 and later resumes from 3.
It does **not** redraw. If it redrew, the two nodes would be equally likely to
collide again; freezing guarantees the deferring node transmits sooner next
time, which is how 802.11 keeps contention from repeating."

Now force an actual collision:

```
collide hard
```

```
[MAC][W] COLLISION TEST: bypassing carrier sense for this frame
[MAC][W] ACK TIMEOUT for seq=42 to N3 (60 ms) - retry 1/4, CW 8 -> 16
                                               (binary exponential backoff)
[MAC][I] TX DATA seq=42 UNICAST N1->N3 ...  [retry 1]
[MAC][I] ACK from N3 for seq=42 (28 ms, 2 attempts) - delivered
```

**Say:** "With carrier sense bypassed the frames genuinely overlap and the ACK
is lost. The timeout fires, the contention window doubles from 8 to 16 slots,
and the retransmission succeeds. So we show both halves: collisions **avoided**
by sensing and backoff, and collisions **recovered from** by timeout and
exponential backoff."

### Step 9 · Dynamic schedule update (required: "not completely static")

Hold a lighter (unlit — just the gas) near N2's MQ2 for a few seconds.

```
N2: [SENS][W] GAS ALARM: 1120 ppm >= 800 ppm threshold
N3: [TDMA][W] SCHEDULE UPDATED to v2: alarm raised - dynamic slot granted (N2)
    --- SCHEDULE v2 ---
     3     750-1000     DATA     N2      listen       <-- was SLEEP
```

**Say:** "Slot 3 is unassigned by default, which makes it a sleep slot. When N2
raises a gas alarm the cluster head grants it that slot, so the node in the
middle of an event reports twice as often. When the alarm clears the slot goes
back to sleep. The schedule tracks demand — it is not on a timer, and every
change carries a version number and a reason."

Let it clear and show `SCHEDULE UPDATED to v3: alarm cleared - dynamic slot released`.

### Step 10 · Unused-slot handling and sleep/wake

```
[TDMA][I] Slot 1 was mine but UNUSED (nothing queued) - released, the cluster
          head may reclaim it
```

Then `sched` on N1:

```
   Sleep slots        : 214   (53500 ms total)
   Radio duty cycle   : 46.3 %  (sleep mode: radio off)
```

**Say:** "A node is awake for the beacon, its own slots, the contention period
and any slot where it might have to receive. Everything else the radio is
powered down with `esp_wifi_stop()` — a real 40 mA saving, not a simulation —
and the LED is off so you can see it from across the bench. N1 is at 46 %
duty cycle. The sink never sleeps: it is mains-powered and has to serve the
dashboard, which is the normal asymmetry in a WSN."

### Step 11 · Data aggregation

**Show:** N3's console.

```
[AGG ][I] Aggregated 3 readings from 3 source(s) [mask 0x07]: 3 packets / 141 B
          -> 1 packet / 75 B  (46.8 % fewer bytes)
[AGG ][I]   temp 28.4/28.4/28.4  hum 55.1/55.1/55.1  gas 212/661/1110 (min/avg/max)
            alarms 0x01
[METRIC][I] AGGREGATION: 3 packets (141 B) -> 1 packet (75 B)  |  cumulative
            reduction: 46.8 % of bytes, 66.7 % of packets
```

**Say:** "N3 holds the frames from N1 and N2 rather than forwarding them, folds
in its own MQ135 reading, and uplinks one aggregate per superframe: three
packets in, one out. We send min, max **and** mean, not just the mean —
if one node reports 1110 ppm and two report 212, the mean of 661 looks
unremarkable while the max is the alarm. For the same reason the alarm bits
are OR-ed: aggregation must never be able to swallow an alarm."

### Step 12 · Multi-hop routing to the sink

**Show:** the dashboard's routing panel, and the sink console.

```
[METRIC] RX from N3 seq=88 hops=1 path 1->3->4
```

**Say:** "Path `1→3→4` — two hops. The route came from a cost function using
all five parameters the brief lists: hop count, RSSI, ACK success rate, packet
loss, and link availability as an infinite cost. ACK rate carries the heaviest
weight, because it is the most direct evidence a link actually delivers —
RSSI can look healthy on a link that is failing."

### Step 13 · **Link failure, detection, recovery** (all five steps required)

Press **Fail N3** on the dashboard, or type `fail 3`. Only N3 is told; its
neighbours are deliberately left to work the failure out for themselves, which
is the step the brief asks you to demonstrate. *(Unplugging N3 physically is
even more convincing — the silence detector catches that too.)*

```
N3: *** SIMULATED NODE FAILURE - THIS NODE IS NOW 'DEAD' ***
N1: [MAC  ][W] ACK TIMEOUT for seq=93 to N3 (60 ms) - retry 1/4, CW 8 -> 16
N1: [MAC  ][W] ACK TIMEOUT for seq=93 to N3 (60 ms) - retry 2/4, CW 16 -> 32
N1: [MAC  ][E] No ACK from N3 for seq=93 after 5 attempts - DROPPING
N1: [ROUTE][E] LINK FAILURE DETECTED: N3 did not acknowledge 3 consecutive
               frames - marking the link DOWN
N1: [ROUTE][W] ALTERNATE ROUTE SELECTED: next hop N3 -> N2
               (2 hops to sink, cost 412, failover 187 ms)
N1: [NODE ][I] Broadcasting route control: LINK_DOWN for link N1-N3
N2: [NODE ][I] RELAYING DATA seq=94 from N1 -> next hop N4 (final N4)
N4: [METRIC] RX from N1 seq=94 hops=1 path 1->2->4
```

Narrate the five required stages as they appear:

1. **Link failure** — N3 stops answering
2. **Failure detection** — three consecutive ACK losses (and, for a physical
   unplug, a 7 s silence timeout, because a dead node we are not transmitting
   to never fails an ACK)
3. **Route update** — cost(N3) → ∞, `ROUTE_UPDATE` broadcast
4. **Alternate route selection** — N2 wins on cost; it becomes a relay
5. **Successful delivery** — the sink keeps receiving, and the path string has
   changed from `1→3→4` to `1→2→4`

Point at the dashboard: the topology graph's green edge has moved.

**Say:** "Failover took 187 ms."

Now press **Recover N3**:

```
N1: [ROUTE][I] Link to N3 RECOVERED (heard again after 4 failures) - recomputing
N1: [ROUTE][W] ROUTE CHANGED: next hop N2 -> N3 (2 hops to sink, cost 268)
                                                 [primary path]
```

**Say:** "Recovery does not *force* the old route back. Hearing N3 only clears
the down flag; the original path then has to win on cost like any other
candidate — which it does, because it has a better RSSI and a clean ACK
history."

### Step 14 · Performance analysis

Type `perf` on the sink, and show the dashboard's metrics row.

```
    1. Packet Delivery Ratio : 96.42 %   (243 received / 252 expected)
    2. Packet loss           : 9 packets  (3.58 %)
    3. End-to-end delay      : 312 ms mean, 814 ms max
    4. Throughput            : 94.6 bit/s  (application payload)
    5. Retransmissions       : 14 (reported by sources)
    6. Localization error    : 0.51 m  (RMSE 0.55 m, node N1, 3 anchors)
    7. Data reduction        : 46.80 % of bytes, 66.67 % of packets
```

**Say:** "All seven metrics; the brief asks for four. PDR comes from gaps in
each source's sequence numbers — that is the honest definition at a receiver,
because it counts packets the sink never heard about. Duplicates are discarded
before counting, otherwise PDR would read above 100 %. Delay is dominated by
the TDMA slot structure, not by the radio: a reading waits up to one
superframe for its slot, which is why a 2-second superframe gives a few
hundred milliseconds of mean delay. That is the deliberate trade — we spend
latency to buy a 46 % duty cycle."

### Step 15 · Generate the graphs

Stop the logger (Ctrl-C) and run:

```bash
python tools/plot_metrics.py
```

Show `results/run_*/summary.md` and the six PNGs.

---

## Quick-reference command card

| Command | Shows |
|---|---|
| `status` | phase, uptime, heap, network view |
| `loc` | RSSI, distances, position, error, RMSE |
| `mac` | CCA deferrals, backoff freezes, retries, ACK rate |
| `sched` | live slot map, version, duty cycle |
| `nbr` / `route` | neighbour table with costs, current path |
| `agg` | aggregation window and data reduction |
| `perf` | all seven metrics |
| `collide` / `collide hard` | contention / forced collision |
| `fail 3` / `recover 3` | route failure and recovery |
| `sensors` | raw ADC, Rs/R0, ppm, health flags |
| `stats` | everything at once |

---

## If something goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| No frames anywhere | `NODE_MAC` table wrong | Re-run `tools/print_mac`; the boot banner also warns |
| Dashboard unreachable | SoftAP channel ≠ ESP-NOW channel | Both are pinned to 1 in `wsn_config.h`; re-flash the sink |
| Non-head nodes never transmit | No beacon heard → never synced | Check N3 is alive; `sched` shows `Synced: NO` |
| Gas reads `n/a` | `R0` never calibrated | `calib` in clean air |
| Gas raw = 0 or 4095 | MQ disconnected or on an ADC2 pin | `sensors` names which; see `docs/WIRING.md` |
| Temp/hum `INVALID` | Missing 10 kΩ pull-up on DHT22 | Add it |
| Localization "needs 3 anchors" | The sink's BLE is off or out of range | Check `SINK_BLE_ANCHOR` is 1 |
| Large localization error | `RSSI@1m` or `n` not calibrated for this room | `calib1m <id>`, then `pathloss 2.2`–`3.0` |
| PDR suddenly poor | Node browning out under Wi-Fi bursts | Better 5 V supply |
| Node stuck "playing dead" | A `fail` command is still in force | `recover <id>` |
