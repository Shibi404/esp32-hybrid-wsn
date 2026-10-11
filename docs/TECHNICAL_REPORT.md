# Design and Implementation of an Advanced Hybrid Wireless Sensor Network using ESP32

### with BLE Localization, Wi-Fi MAC, Dynamic Scheduling, Data Aggregation and Adaptive Routing

**23CSE341 — Wireless Sensor Networks · Lab Evaluation 2**

| | |
|---|---|
| **Student** | ____________________ |
| **Roll number** | ____________________ |
| **Date** | ____________________ |
| **Repository** | <https://github.com/Shibi404/esp32-hybrid-wsn> |

---

## Abstract

A four-node wireless sensor network was designed and built on ESP32 hardware,
integrating BLE-based localization with a Wi-Fi data plane in a single running
system. The BLE subsystem performs neighbour discovery, RSSI measurement,
distance estimation and least-squares trilateration, and scores its result
against a tape-measured ground truth. The Wi-Fi subsystem implements an
application-layer CSMA/CA MAC (physical and virtual carrier sense, DIFS,
random backoff with freezing, acknowledgement, timeout, binary exponential
backoff), a dynamic TDMA superframe with demand-driven slot allocation and
radio sleep, cluster-head data aggregation, and adaptive multi-hop routing
with failure detection and recovery. All seven performance metrics listed in
the evaluation guidelines are measured, against a requirement of four. A
browser dashboard served by the sink visualises node status, localization,
sensor data, routing path, aggregation, MAC activity and performance in real
time.

The protocol layers are implemented in this project rather than inherited from
a library, so that each mechanism is observable and can be demonstrated.

---

## 1. Objective

Design and implement a four-node WSN on ESP32 integrating BLE-based
localization, a Wi-Fi MAC protocol, broadcast and unicast communication,
dynamic schedule-based communication, data aggregation, multi-hop routing,
route failure and recovery, and performance measurement — demonstrating how
these protocols operate together in one network rather than as separate
exercises.

---

## 2. Node configuration

| Node | Role | Sensor | Major functions |
|---|---|---|---|
| **N1** | Sensor | DHT22 (temperature, humidity) | Sensing, BLE mobile node (self-localizing), data origination, CSMA/CA, TDMA slot 1 |
| **N2** | Sensor / Relay | MQ2 (smoke, LPG) | Sensing, BLE anchor, data origination, **forwarding for N1 on the alternate route** |
| **N3** | Cluster head / Aggregator / Router | MQ135 (air quality) | BLE anchor, TDMA scheduler and time master, **data aggregation**, routing, forwarding |
| **N4** | Sink / Gateway | HC-SR501 (PIR motion) | BLE anchor, data collection, metric computation, **SoftAP + dashboard** |

Addressing is two-layer: a 6-byte MAC per peer for transmission, and a logical
node id (1–4) carried in every frame header, which is what the routing,
scheduling and metric code operates on. This separation is what allows a route
to be recorded and printed as `1→3→4`.

### 2.1 Hybrid architecture and the single-radio constraint

The ESP32 has one 2.4 GHz radio shared between Wi-Fi and Bluetooth.

Advertising is a short burst every few tens of milliseconds and coexists with
Wi-Fi without trouble. **Scanning** is a continuous receive, and that is what
genuinely conflicts on a shared radio — it both starves the scan window and
biases the RSSI the localization depends on. The split follows from that:

* **Every node advertises, always.** That is what makes an anchor an anchor:
  discoverable whenever somebody looks. It also removes a phase-alignment
  problem — boards booted at different times would otherwise drift into
  different phases and a scan could find no anchors at all.
* **Only N1 and N2 scan**, pausing their network participation to do it. This
  matches the node table in the brief exactly, where BLE is listed against N1
  and N2 but not against the cluster head or the sink.
* **N3 and N4 never scan.** N3 is the time master and scheduler — blocking it
  for three seconds would stop the whole superframe. N4 must keep serving the
  dashboard.

A scanning node is off the air for about 3 s, roughly 1.5 superframes. The
slot-revocation threshold is 4 superframes, comfortably clear of it, and the
node waits for its transmit queue to drain before it leaves, so no frame is
abandoned mid-handshake.

N1 and N2 therefore alternate two phases, in the order the evaluation
guidelines specify:

```
   PHASE A · BLE scan (~3.5 s)   PHASE B · Wi-Fi  (~30 s)
   active scan              →    beacon / schedule / time sync
   RSSI → median → EWMA          CSMA/CA → TDMA transmission
   → distance                    aggregation at N3
   → trilateration               multi-hop → sink
   → error, RMSE                 metrics, dashboard, sleep
            ↑                                      │
            └──────────────────────────────────────┘
```

Both stacks remain initialised throughout — ESP-NOW peer registrations, the
sink's SoftAP and BLE advertising all persist across phases; only the scan
comes and goes. N3 and N4 have no Phase A: the cluster head must keep the
superframe running, and the sink must keep the dashboard reachable.

### 2.2 Where the two subsystems actually interact

A frequent weakness in a "hybrid" design is that the two halves are
demonstrated independently and never touch. Three couplings exist here:

1. **BLE RSSI feeds the routing metric.** `routeNoteBleRssi()` injects the
   filtered BLE RSSI into the route cost function. Before a neighbour's Wi-Fi
   RSSI is known, the BLE measurement is what selects the next hop.
2. **The localization result travels over Wi-Fi.** N1 computes its position
   over BLE and reports it to the sink as an `FT_LOC` frame, so the
   dashboard's localization panel is fed through the Wi-Fi data plane.
3. **The phase machine is itself a coupling.** Wi-Fi traffic is suppressed
   during ranging so RSSI is measured on a quiet medium.

---

## 3. Hardware and circuit

| Node | Sensor | Pin | Supply | Note |
|---|---|---|---|---|
| N1 | DHT22 DATA | GPIO 4 | 3V3 | 10 kΩ pull-up to 3V3 required |
| N2 | MQ2 AOUT | GPIO 34 (ADC1_CH6) | **3V3** | input-only pin |
| N3 | MQ135 AOUT | GPIO 35 (ADC1_CH7) | **3V3** | input-only pin |
| N4 | HC-SR501 OUT | GPIO 27 | 5V/VIN | 3.3 V TTL output; trigger jumper `H` |
| all | status LED | GPIO 2 | — | lit during the node's own TDMA slot |

Two constraints governed these choices.

**ADC2 is unusable while Wi-Fi is active.** The Wi-Fi driver owns ADC2, so an
analog read on GPIO 0/2/4/12–15/25–27 returns invalid data with no error
indication. Both MQ analog outputs are therefore on ADC1 (GPIO 32–39).

**The MQ modules are powered from 3.3 V, not 5 V.** The potentiometer on an
MQ breakout drives the LM393 comparator behind the digital `DOUT` pin and does
not attenuate the analog `AOUT` line, which swings to `VCC`. Running the
modules at 3.3 V keeps `AOUT` permanently within the ESP32's input range
without additional components. Section 9 states what this costs.

ADC conditioning uses 11 dB attenuation, 16× multisampling and the core's
factory-calibrated `analogReadMilliVolts()`, which compensates the ESP32 SAR
ADC's known non-linearity.

Full wiring, including the 5 V + divider upgrade path, is in `docs/WIRING.md`.

---

## 4. Protocol design

### 4.1 Frame format

```
  21-byte header                                       payload      CRC-16
 ┌──────┬──────┬──────┬─────┬─────┬──────────┬─────┬─────┬─────┬──────────┐
 │ MAGIC│ VER  │ TYPE │ SRC │ DST │ FINAL_DST│ HOP │ TTL │ SEQ │ txTimeMs │
 ├──────┴──────┴──────┴─────┴─────┴──────────┴─────┴─────┴─────┴──────────┤
 │ durationUs │ pathLen │ path[4] │            payload           │ CRC-16 │
 └────────────┴─────────┴─────────┴──────────────────────────────┴────────┘
```

| Field | Rationale |
|---|---|
| `dst` vs `finalDst` | Next hop and destination are separate fields; this is what makes multi-hop forwarding possible at all. |
| `seq` | Per-originator sequence; the sink derives PDR and loss from gaps in it. |
| `txTimeMs` | Network time at **origination**, never re-stamped on a retry, so the delay measurement includes the cost of retransmissions. |
| `durationUs` | Network Allocation Vector: the medium occupancy the sender declares. Overhearing nodes treat the channel as busy for that long. |
| `path[4]` | Nodes traversed — gives the sink a printable route and lets a forwarder drop a looping frame. |
| CRC-16 | CCITT. ESP-NOW already carries an 802.11 FCS, but the forced-collision experiment deliberately produces overlapping transmissions, and a truncated frame must never be interpreted as sensor data. |

Frames whose declared type does not match the received payload length are
rejected before parsing, so a short frame can never be read past its end.

### 4.2 Broadcast and unicast

| Cast | Frames | Why |
|---|---|---|
| **Broadcast** | `BEACON` (schedule), `ROUTE_UPDATE` (route control), `NET_STATUS` (network status), `HELLO` (discovery), `CMD` | Information every node needs, repeated next superframe regardless. Acknowledging it to four receivers would cost four ACKs for data that is already refreshed periodically. |
| **Unicast** | `DATA`, `AGG`, `LOC`, `SLOT_REQ`, `ACK` | Anything requiring guaranteed, acknowledged delivery to one specific node. |

This matches the mapping the guidelines specify exactly.

---

## 5. BLE localization

### 5.1 Chain

```
  advertise "WSN-Nx" + active scan (duplicates enabled)
        ↓
  ≥3 RSSI samples per neighbour  →  median  →  EWMA (α = 0.3)
        ↓
  d = 10^((RSSI₁ₘ − RSSI) / (10·n))          log-distance path loss
        ↓
        ├──→ argmax RSSI  =  nearest node / proximity
        ↓
  least-squares trilateration from 3 anchors  →  (x̂, ŷ)
        ↓
  error = ‖(x̂, ŷ) − ground truth‖ ;  running mean and RMSE
```

Duplicate reporting is enabled in the scanner deliberately. The controller's
default duplicate filter would yield one advertisement per neighbour per
round — far too few samples to median-filter.

### 5.2 Why median then EWMA

BLE RSSI in a 2.4 GHz environment exhibits occasional 15–20 dB excursions from
multipath and Wi-Fi interference. An arithmetic mean is dragged by these
outliers; a median rejects them outright while they remain a minority of
samples. The EWMA applied across rounds then prevents the position estimate
jittering between frames. Both filters are necessary: the median handles
outliers within a round, the EWMA handles variance between rounds.

### 5.3 Trilateration

Each anchor *i* gives a circle `(x − xᵢ)² + (y − yᵢ)² = dᵢ²`. Subtracting the
reference anchor's equation from each of the others cancels the quadratic
terms, leaving a linear system solved in the least-squares sense via the 2×2
normal equations `(AᵀA)p = Aᵀb`. With three anchors the system is exactly
determined; with more it becomes overdetermined and least squares averages out
ranging noise.

A determinant near zero indicates collinear anchors, which cannot fix a 2-D
position — the circles then intersect in a pair of points mirrored about the
line. This is detected and reported rather than returning an extrapolated
answer.

`RSSI₁ₘ` and the path-loss exponent `n` are calibrated in the deployment room
(`calib1m`, `pathloss`) and persisted to NVS.

---

## 6. Wi-Fi MAC protocol

ESP-NOW rides on 802.11, which already performs CSMA/CA in silicon. A second,
visible MAC is implemented at the application layer because the requirement is
to demonstrate the mechanism — every decision below is logged and counted.
This mirrors how real WSN MACs such as S-MAC and B-MAC sit above the radio.

```
  [1] CARRIER SENSE   physical CCA  — promiscuous sniffer timestamps every
                                      802.11 frame above −75 dBm; medium busy
                                      for 3 ms afterwards
                   OR virtual (NAV) — honour the durationUs every WSN frame
                                      declares (covers hidden terminals that
                                      physical sensing misses)
  [2] DIFS            medium must remain idle 2 ms continuously
  [3] BACKOFF         rand(0, CW) slots of 500 µs; the counter FREEZES when
                      the medium goes busy and RESUMES where it stopped
  [4] TRANSMIT        unicast to the next hop; set the local NAV
  [5] ACK / TIMEOUT   60 ms. ACK → CW := CW_MIN, record RTT, credit the link.
                      Timeout → CW := min(2·CW, 64), retry up to 4 times, then
                      drop and debit the link so routing reacts.
```

### 6.1 Why freezing matters

This is the central detail of the contention mechanism. If two deferring nodes
both re-drew their backoff, they would be equally likely to collide again on
the next attempt. By freezing, the node that has already counted down furthest
resumes closest to zero and therefore transmits first — contention is resolved
monotonically instead of being re-randomised. `backoffFreezes` counts how often
this occurred, and it is non-zero in any normal run because all four nodes
contend for the CAP slot every superframe.

### 6.2 Acknowledgement

The receiver of a unicast data-bearing frame returns an ACK after SIFS
(300 µs) **without contending**, as in 802.11 — an ACK forced to contend could
be beaten by a new transmission, defeating its purpose. Broadcasts are never
acknowledged.

### 6.3 Starvation and link feedback

If the medium stays busy for longer than 50 ms the frame is transmitted anyway
and counted as a forced transmission, so a node can never be starved
indefinitely in a saturated band. When a frame is finally dropped at the retry
limit, `routeReportTxResult()` is invoked — MAC failure feeds the routing
metric rather than being silently absorbed.

### 6.4 Collision demonstration

The cluster head can assign two owners to the contention slot, so N1 and N2
have a frame ready at the same instant. Only the *timing* is staged; the
carrier sense, the random draws, the freeze and the ACK are all the real
mechanism. A second mode (`collide hard`) bypasses carrier sense so a genuine
collision occurs and the timeout, contention-window doubling and successful
retransmission can be observed.

---

## 7. Dynamic schedule-based communication

```
  ├────────────────────── 2000 ms superframe ──────────────────────┤
  │  0      │ 1    │ 2    │ 3       │ 4    │ 5   │ 6      │ 7      │
  │ BEACON  │ DATA │ DATA │ DYNAMIC │ AGG  │ CAP │ STATUS │ SLEEP  │
  │  N3     │ N1   │ N2   │ (none)  │ N3   │ all │  N4    │  —     │
```

### 7.1 Time synchronisation

N3's beacon carries its own clock. Each node computes
`offset = beaconTime − localMillis()` and derives a shared network time.
Propagation over a few metres is far below one millisecond and is ignored; the
dominant error is receive-queue latency, which is why the beacon is processed
promptly in the receive path. Guard bands of 20 ms at each slot edge absorb
the residual skew.

This shared clock is also what makes the end-to-end delay measurement valid:
the timestamp is written by one board and read by another, so without it the
subtraction would measure the difference between two unrelated boot times.

### 7.2 What makes the schedule dynamic

Slot 3 is unassigned by default and is therefore a sleep slot. The cluster
head re-evaluates the schedule every superframe:

| Trigger | Action | Reason code |
|---|---|---|
| A node's gas or motion alarm asserts | grant slot 3 to it | `ALARM_GRANT` |
| The alarm clears | return slot 3 to sleep | `ALARM_RELEASE` |
| A node reports a backlog ≥ 3 frames | grant slot 3 to it | `BACKLOG_GRANT` |
| A node misses 3 consecutive slots | revoke its slot | `NODE_SILENT` |
| A revoked node transmits again | restore its slot | `NODE_REJOIN` |

The schedule tracks demand; it is not driven by a timer. Every change
increments `schedVersion` and is broadcast with a reason code that prints in
plain English. Version numbering also makes adoption safe — nodes adopt only a
differing version and the head only increments, so a stale beacon overheard
late cannot roll the network backwards.

### 7.3 Unused slots and sleep

A node that owns a slot but has nothing queued when the slot ends logs it as
unused; the counter feeds the head's reclamation logic, and no airtime is
spent on an empty transmission.

Sleep is real rather than simulated. `esp_wifi_stop()` powers down the Wi-Fi
MAC and PHY — roughly 40 mA on an ESP32 — and the radio is restarted before
the next slot in which the node must transmit or receive. The CPU remains
awake so the serial log continues to narrate, which is the right trade for a
demonstration. `ENABLE_LIGHT_SLEEP` switches to `esp_light_sleep_start()`,
sleeping the whole SoC to under a milliamp; `millis()` survives it because
`esp_timer` is RTC-backed.

A node is awake for the beacon slot, its own slots, the contention period, and
any slot in which it may have to receive. The sink never sleeps: it is
mains-powered and must keep the dashboard reachable. This asymmetry — energy
scarce at the leaves, not at the gateway — is standard WSN design.

**A measured constraint shapes this.** `esp_wifi_start()` requires roughly
100 ms to bring the PHY back, which is 40 % of a 250 ms slot. In testing, a
node that woke on the slot boundary was still booting its radio when that
slot's traffic arrived: ACK loss reached 60–85 % on links measuring −54 dBm,
and the resulting phantom link failures drove 70 route changes in a few
minutes. A node therefore sleeps through a slot only when the following slot
is also idle, which leaves the last idle slot of every run awake as a warm-up
slot. The achievable duty cycle falls from a theoretical 50 % to a measured
75 %, and the network becomes reliable. The cluster head stops sleeping
altogether as a consequence — neither of its idle slots is followed by
another — which is correct, since a scheduler that wakes into its own beacon
slot with a cold radio desynchronises the network. `ENABLE_LIGHT_SLEEP`
recovers most of the difference, light sleep resuming in milliseconds rather
than restarting the driver.

---

## 8. Data aggregation

N3 holds the `DATA` frames received from N1 and N2 within a superframe rather
than forwarding them, folds in its own MQ135 reading, and uplinks a single
aggregate.

**Aggregation function:** per field (temperature, humidity, gas) the minimum,
maximum and mean, with a per-field sample count; plus the bitwise OR of all
contributors' alarm bits and a bitmap of contributing sources.

**Why not the mean alone.** A mean conceals the event the network exists to
detect. With gas readings of 1110, 212 and 212 ppm the mean is 661 — entirely
unremarkable — while the maximum, 1110, is the alarm. Transmitting min and max
costs 8 additional bytes and preserves the information that matters. The same
reasoning governs the alarm bits: they are OR-ed, so aggregation can never
swallow an alarm.

**Sensor faults.** A field a contributor marked invalid is excluded from the
statistics rather than counted as zero. Per-field sample counts make this
possible, so a node with one dead sensor still contributes its working ones.

**Reduction:**

```
  3 × DATA = 3 × (21 + 24 + 2) = 3 × 47 = 141 B,  3 packets
  1 × AGG  = 1 × (21 + 52 + 2) = 1 × 75 =  75 B,  1 packet

  byte reduction   = (1 −  75/141) × 100 = 46.8 %
  packet reduction = (1 −   1/3  ) × 100 = 66.7 %
```

Packet reduction is the more meaningful figure in a WSN: each transmission
costs a backoff, a preamble, an ACK and a receive window regardless of payload
size, so per-packet overhead dominates energy and airtime. Byte reduction is
reported as the conservative number.

---

## 9. Adaptive multi-hop routing

### 9.1 Route metric

```
  cost = 100·(hops + 1)          hop count
       + 150·(1 − rssiNorm)      RSSI / link quality
       + 200·(1 − ackRate)       successful ACK rate
       + 100·lossRate            packet loss
       = ∞  when the link is down    link availability

  rssiNorm maps −95 dBm → 0 and −35 dBm → 1
```

All five parameters named in the guidelines are used. ACK rate carries the
heaviest weight because it is the most direct evidence that a link actually
delivers: a link can present −55 dBm and still fail under interference or at a
congested receiver. Hop count is the weakest indicator and is weighted least.

Two further terms shape the outcome.

**Cluster-head preference** (−150). A sensor node reporting to its cluster head
is what makes aggregation possible at all; routing around the head would carry
the same readings to the sink in three times as many packets. The term is a
discount rather than a hard rule, so when the head's link degrades or fails the
arithmetic allows an ordinary neighbour to win. This mirrors cluster-member
behaviour in LEACH.

**Topology constraint.** All four boards sit within radio range on one bench, so
without a constraint every node would reach the sink in a single hop and the
multi-hop forwarding, aggregation and route-recovery mechanisms would never
execute. `ROUTE_LINK_ALLOWED` declares the topology the deployment is intended
to have — N1 has no direct link to the sink. This is a routing policy, stated
explicitly rather than concealed, and it is not a claim about what the radios
can hear: distributing the nodes along a corridor would produce the same
topology physically, and setting every entry to 1 lets the cost metric discover
it unaided.

The RSSI input prefers the Wi-Fi RSSI harvested by the MAC's promiscuous
sniffer — which parses `addr2` from the 802.11 header, making it work
identically on Arduino core 2.x and 3.x despite their incompatible ESP-NOW
receive callbacks — and falls back to the BLE RSSI from the localization
phase.

### 9.2 Neighbour table

Each node maintains, per neighbour: link state, Wi-Fi RSSI, BLE RSSI,
advertised hop count, role, transmit attempts, ACKs received, consecutive
failures, frames received, and the computed cost. Costs are recomputed on
every sweep, not only after a failure, so the route tracks drifting link
quality — adaptive rather than merely reactive.

### 9.3 Failure detection and recovery

Two independent detectors operate, because they catch different failures:

- **Consecutive ACK loss** (3 failures) catches a peer we are actively
  transmitting to.
- **Silence timeout** (7 s) catches a neighbour that was powered off and that
  we were never transmitting to — such a node would never fail an ACK, so the
  first detector would never fire.

The five stages the guidelines require:

| Stage | Implementation |
|---|---|
| 1. Link failure | N3 powered off, or `fail 3` makes it stop responding |
| 2. Failure detection | 3 consecutive ACK losses, or 7 s of silence |
| 3. Route update | `cost(N3) := ∞`; `ROUTE_UPDATE` broadcast |
| 4. Alternate route selection | next-lowest cost wins: `N1 → N2 → N4`; N2 becomes a relay |
| 5. Successful delivery | the sink continues to receive; the path string changes from `1→3→4` to `1→2→4` |

**Recovery** does not force the original route back. Hearing N3 again only
clears the down flag; the original path must then win on cost against the
current alternative — which it normally does within a second, because it has
better RSSI and a clean ACK history, but it wins on evidence rather than by
preference.

**Loop prevention** uses three mechanisms: a TTL bounding any loop to 5 hops,
the `path[]` list so a forwarder refuses to hand a frame to a node already in
its path, and advertised hop counts so a neighbour routing through us does not
appear attractive.

Failover latency (detection → alternate route) is measured and reported.

---

## 10. Performance measurement

| # | Metric | Measured at | Method |
|---|---|---|---|
| 1 | Packet Delivery Ratio | sink | received ÷ observed sequence span, per source |
| 2 | Packet loss | sink | gaps in the sequence numbers |
| 3 | End-to-end delay | sink | `netTime − txTimeMs`, using the beacon-synced clock |
| 4 | Throughput | sink | application payload bits ÷ elapsed seconds |
| 5 | Retransmissions | all nodes | MAC retry counter, carried in each data frame |
| 6 | Localization error | N1 | distance from estimate to ground truth, with RMSE |
| 7 | Data reduction | N3 | `1 − aggBytes ÷ rawBytes`, bytes and packets |

Four are required; all seven are implemented.

Three measurement decisions are worth stating.

`expected` is derived from the observed sequence span rather than from a count
the source claims to have sent — that is the honest definition at a receiver,
because it counts packets the sink never heard about. Duplicates are detected
and discarded before counting, since otherwise PDR could exceed 100 %.

Data-bearing frames carry their own sequence space, separate from HELLOs and
beacons. Sharing one counter would make every data frame appear to be preceded
by several lost ones, and PDR would read far below its true value.

While aggregation is working, a sensor node's raw packets stop at the cluster
head by design and never reach the sink, so its *direct* PDR is not its
delivery evidence. The sink instead credits each contributor named in a
delivered aggregate's source mask, and reports that as a separate column.

Delay samples taken before the clocks converge (which can occur for the first
frame after a resync) are discarded rather than folded in as zero.

**Measured results are in `docs/RESULTS.md`**, with figures generated by
`tools/plot_metrics.py` from a captured run.

---

## 11. Real-time dashboard

The sink hosts a SoftAP (`WSN_SINK`, channel 1) and serves a single-page
dashboard at `http://192.168.4.1` with the seven panels the guidelines
require: node status, BLE localization (including a to-scale map of anchors,
estimate and ground truth), sensor data, routing path (a live topology graph
whose active edges move during a failover), aggregated data, MAC activity, and
key performance metrics — plus a live slot map and demonstration controls.

The SoftAP runs on the same channel as ESP-NOW because one radio serves both;
a channel mismatch leaves the AP working while ESP-NOW reception silently
stops.

The page is embedded as a `PROGMEM` string rather than uploaded to a
filesystem, so flashing the sketch is the entire deployment. JSON is
hand-rolled, keeping the external dependency list to two Adafruit sensor
libraries and removing a class of library-version failures. Invalid sensor
readings serialise as `null` rather than `0`, so the page cannot display a
fabricated value.

---

## 12. Software organisation

| Module | Responsibility |
|---|---|
| `wsn_config` | MAC table, anchor coordinates, pin map, all tuning constants |
| `wsn_protocol` | Frame format, CRC-16, path handling |
| `wsn_log` | Human-readable stream and a parallel CSV stream on one UART |
| `wsn_sensors` | DHT22 / MQ2 / MQ135 / PIR, warm-up, calibration, fault handling |
| `wsn_ble` | Advertising, scanning, filtering, ranging, trilateration |
| `wsn_mac` | CSMA/CA state machine |
| `wsn_tdma` | Superframe, dynamic slots, time sync, sleep/wake |
| `wsn_route` | Neighbour table, cost metric, failure detection, recovery |
| `wsn_aggregate` | Min/max/mean fusion and reduction accounting |
| `wsn_metrics` | All seven performance metrics |
| `wsn_node` | Phase machine, slot actions, frame dispatch — shared by all roles |
| `wsn_cli` | Serial command interface |
| `dashboard_html` | The sink's web dashboard |

`NODE_ID` and `NODE_ROLE` are runtime globals rather than compile-time
constants. Arduino compiles each `.cpp` as a separate translation unit, so a
`#define` in the `.ino` would not reach the shared modules; keeping them as
externs means `common/` is byte-identical across all four sketches.
`tools/sync_common.py` maintains that property and `--check` fails if any copy
has drifted.

---

## 13. Limitations

Stated explicitly rather than omitted.

**Gas concentration is indicative, not calibrated.** The MQ heaters run at
3.3 V instead of the rated 5 V, so the sensing element is below its intended
operating temperature and the datasheet curve `ppm = a·(Rs/R0)^b` does not
strictly apply. The sensors have also had no 24–48 hour burn-in, so the
baseline continues to drift. `R0` is calibrated on this hardware at this
heater voltage and stored in NVS, so the `Rs/R0` ratio is self-consistent and
the relative response — which is what the alarm logic and the demonstration
depend on — is reliable and repeatable. Accurate absolute ppm requires a 5 V
supply with a 10 kΩ/10 kΩ divider on each analog output and a one-line change
to `MQ_DIVIDER_GAIN`.

**Localization accuracy is bounded by RSSI ranging.** Everything downstream —
the position, the error, the RMSE — inherits the noise of an amplitude-based
distance estimate in a multipath environment, and no amount of filtering
removes it. A single global path-loss exponent also cannot capture
direction-dependent attenuation. With only three anchors there is no
redundancy and no way to reject a bad range. Meaningful improvement requires
a time-based ranging method (UWB time-of-flight) or angle-of-arrival.

**Time synchronisation is one-way.** A single beacon offset with no
round-trip correction is adequate at 20 ms guard bands but would not scale to
substantially shorter slots.

**The collision demonstration is staged in timing only.** The cluster head
grants two nodes the same slot; everything after that — carrier sense, the
random draws, the freeze, the ACK — is the real mechanism. Unstaged contention
also occurs in every CAP slot.

**The multi-hop topology is declared, not physical.** With all four boards on
one bench, every node is within range of the sink. `ROUTE_LINK_ALLOWED`
expresses the intended deployment topology so that the multi-hop mechanisms
actually execute. The routing, failure detection and recovery that follow are
entirely genuine — only the adjacency is asserted, and spacing the nodes out
would make it physical.

**Duty cycling is limited by radio restart latency.** The 100 ms cost of
`esp_wifi_start()` against a 250 ms slot is what caps the achievable duty
cycle at 75 %. A longer superframe, or light sleep instead of a driver
restart, would both improve it.

**No hidden-terminal protection beyond NAV.** The virtual carrier sense relies
on overhearing data frames; a proper RTS/CTS exchange would cover the case
where two senders cannot hear each other at all.

---

## 14. Conclusion

A four-node hybrid BLE + Wi-Fi wireless sensor network was implemented in
which eight distinct WSN mechanisms operate together in a single system rather
than as isolated demonstrations. BLE localization feeds the Wi-Fi routing
metric; MAC-layer delivery failure drives route recovery; sensor alarms drive
dynamic slot allocation; and aggregation at the cluster head reduces network
traffic by 47 % of bytes and 67 % of packets while preserving the extremes and
alarms that make the data actionable.

The protocol layers are implemented rather than inherited, which is what makes
each mechanism observable: every carrier-sense decision, backoff draw, freeze,
acknowledgement, slot grant, route cost and recovery event is logged, counted
and surfaced on a live dashboard. All seven performance metrics are measured
against a requirement of four.

---

## 15. References

1. Espressif Systems, *ESP-NOW User Guide*, ESP-IDF Programming Guide v5.x.
2. Espressif Systems, *ESP32 Technical Reference Manual* — ADC2 and Wi-Fi
   coexistence; Bluetooth/Wi-Fi radio coexistence.
3. IEEE Std 802.11-2020, §10.3 — DCF: carrier sense, DIFS/SIFS, random
   backoff with freezing, binary exponential backoff, acknowledgement.
4. IEEE Std 802.15.4 — superframe structure: contention-free period and
   contention access period.
5. W. Ye, J. Heidemann, D. Estrin, "An Energy-Efficient MAC Protocol for
   Wireless Sensor Networks" (S-MAC), *IEEE INFOCOM*, 2002.
6. W. Heinzelman et al., "Energy-Efficient Communication Protocol for Wireless
   Microsensor Networks" (LEACH), *HICSS*, 2000 — cluster-head aggregation.
7. T. S. Rappaport, *Wireless Communications: Principles and Practice*, 2nd
   ed. — log-distance path-loss model.
8. Hanwei Electronics, *MQ-2* and *MQ-135* gas sensor datasheets.
9. Aosong Electronics, *DHT22 / AM2302* datasheet.

---

## Appendix A — Deliverables

| Deliverable | Location |
|---|---|
| Circuit / network setup | `docs/WIRING.md` |
| ESP32 source code | `common/`, `firmware/` |
| Architecture diagram | `docs/ARCHITECTURE.md` |
| Flowchart | `docs/FLOWCHART.md` |
| Output screenshots | `docs/images/` (see `docs/SCREENSHOT_CHECKLIST.md`) |
| Performance results | `docs/RESULTS.md`, `results/run_*/` |
| Technical report | this document |
| Real-time dashboard | `firmware/Node4_Sink/dashboard_html.h` |

## Appendix B — Requirement coverage

| Rubric | Marks | Where implemented |
|---|---|---|
| 1 · WSN architecture and ESP32 setup | 2 | `wsn_config`, `wsn_node`, §2 |
| 2 · BLE-based localization | 3 | `wsn_ble`, §5 |
| 3 · Wi-Fi MAC protocol | 3 | `wsn_mac`, §6 |
| 4 · Dynamic schedule-based communication | 2 | `wsn_tdma`, §7 |
| 5 · Broadcast and unicast | 1 | `wsn_protocol`, `wsn_node`, §4.2 |
| 6 · Data aggregation | 2 | `wsn_aggregate`, §8 |
| 7 · Adaptive multi-hop routing | 2 | `wsn_route`, §9 |
| 8 · Performance analysis | 2 | `wsn_metrics`, §10, `docs/RESULTS.md` |
| 9 · Technical demonstration and viva | 3 | `docs/DEMO_SCRIPT.md`, `docs/VIVA_QA.md` |
