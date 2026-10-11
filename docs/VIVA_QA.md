# Viva Preparation

**23CSE341 WSN — Lab Evaluation 2** · Rubric item 9 (3 marks)

Answers are tied to specific code so you can point at a file while answering.
Where a design has a real weakness, the answer says so — an examiner respects
a known limitation far more than a confident hand-wave.

---

## A · Architecture and setup

**Q. Why ESP-NOW instead of normal Wi-Fi with TCP or UDP?**
ESP-NOW is connectionless and needs no access point, so the network has no
infrastructure dependency — nothing to fail on demo day. It gives direct
MAC-level addressing, which is what a WSN link layer actually looks like, and
it has low per-frame overhead. Using TCP would also hide exactly the things we
are being graded on: retransmission and acknowledgement would happen inside
the stack where we cannot show them.

**Q. Is ESP-NOW really "Wi-Fi"?**
Yes — it is carried in 802.11 vendor-specific action frames on a normal Wi-Fi
channel, using the same PHY and the same hardware CSMA/CA. It is Wi-Fi without
association.

**Q. How are nodes addressed?**
Two layers. Physically by 6-byte MAC (`NODE_MAC[]` in `wsn_config.cpp`), and
logically by a node id 1–4 carried in every frame header. The logical id is
what the routing, scheduling and metrics code works with; the MAC is only used
at the moment of transmission. That separation is why a path can be printed as
`1→3→4`.

**Q. Why does `common/` get copied into each sketch folder?**
Arduino compiles only files sitting next to the `.ino` and offers no include
path to a sibling directory. The usual workaround — copying by hand —
guarantees the four nodes drift apart. `tools/sync_common.py` makes `common/`
the single source of truth and `--check` fails if any copy has drifted.

---

## B · BLE localization (3 marks)

**Q. How do you turn RSSI into distance?**
The log-distance path-loss model:
`RSSI(d) = RSSI(1 m) − 10·n·log₁₀(d)`, rearranged to
`d = 10^((RSSI₁ₘ − RSSI) / (10n))`.
`RSSI₁ₘ` is measured in the actual room with `calib1m`, and `n` is the
path-loss exponent — 2.0 in free space, 2.5–3.5 indoors with walls and
furniture. `wsn_ble.cpp`, `bleRssiToDistance()`.

**Q. Why the median and not the mean of your RSSI samples?**
BLE RSSI in a 2.4 GHz environment shows occasional 15–20 dB outliers from
multipath and Wi-Fi interference. A mean is dragged by them; a median ignores
them entirely as long as they are a minority of samples. We then apply an EWMA
across rounds (α = 0.3) so the position estimate does not jitter.

**Q. Explain your trilateration.**
Each anchor gives a circle `(x−xᵢ)² + (y−yᵢ)² = dᵢ²`. Subtracting the
reference anchor's equation from the others cancels the `x²` and `y²` terms
and leaves a linear system `A·p = b`, which we solve by the 2×2 normal
equations `(AᵀA)p = Aᵀb`. With three anchors it is exactly determined; with
more it is overdetermined and least squares averages out ranging noise.

**Q. What if your anchors are in a straight line?**
The system becomes singular — collinear anchors cannot fix a 2-D position,
because the circles intersect in a pair of points mirrored about the line. We
test `|det| < 1e-9` and report the condition rather than returning an
extrapolated answer.

**Q. How is localization error defined?**
Euclidean distance between the estimate and the tape-measured ground truth
in `NODE_POS[1]`: `err = √((x̂−x)² + (ŷ−y)²)`. We report the per-round error,
the running mean, and the RMSE across all rounds.

**Q. Your error is around half a metre. Why not better?**
Three reasons, in order of size: (1) RSSI-based ranging is fundamentally noisy
— a few dB of fading maps to tens of centimetres; (2) the path-loss exponent
is a single scalar for the whole room, but attenuation is direction-dependent;
(3) three anchors in a small, near-right-angled triangle give modest geometric
dilution of precision. Fixing it properly needs more anchors, or a ranging
method with a time basis rather than an amplitude basis — BLE 5.1 angle-of-
arrival, or UWB time-of-flight.

**Q. What is "nearest-node identification" and how is it different?**
It is proximity rather than position: `argmax` of the filtered RSSI. It needs
only one neighbour, works when trilateration cannot run, and is what many real
deployments actually use ("which room is this tag in?").

**Q. One radio serves BLE and Wi-Fi. How do you stop them interfering?**
By separating advertising from scanning. Advertising is a ~1 ms burst every
few tens of milliseconds and coexists with Wi-Fi without trouble, so every
node advertises continuously — which is also what makes an anchor usable, it
has to be discoverable whenever somebody looks. Scanning is a continuous
receive, and that is what actually conflicts: it starves the scan window and
biases the RSSI we are trying to measure. Only N1 and N2 scan, and they pause
network participation while they do. That matches the brief's node table,
which lists BLE against N1 and N2 but not against N3 or N4.

**Q. Why don't N3 and N4 scan?**
N3 is the time master and scheduler; blocking it for three seconds would stop
the superframe for everybody. N4 must keep the dashboard served. Both still
advertise, so both are still full localization anchors — advertising is all an
anchor has to do.

**Q. What happens to a node's TDMA slot while it is scanning?**
It is off the air for about 3 s, roughly 1.5 superframes, and its slot is
logged as unused. The slot-revocation threshold is 4 superframes, so it keeps
its slot. It also waits for its transmit queue to drain before leaving, so no
frame is abandoned mid-handshake and no retries are wasted. This is ordinary
duty-cycling behaviour, and the unused-slot logging it produces is exactly
what the brief asks us to demonstrate.

---

## C · Wi-Fi MAC (3 marks)

**Q. ESP-NOW already has CSMA/CA in hardware. Why write your own?**
Because the hardware one is invisible. The brief asks us to *implement and
demonstrate* channel sensing, backoff, ACK, timeout, retransmission and
collision handling. Ours logs every decision and exposes counters for all of
them. The hardware CSMA/CA still runs underneath; ours is an application-layer
protocol on top, exactly as a real WSN MAC like S-MAC or B-MAC sits above the
radio.

**Q. How do you sense the channel?**
Two independent mechanisms, OR-ed:
- **Physical CCA** — a promiscuous-mode sniffer callback timestamps every
  802.11 frame it sees; the medium counts busy for 3 ms after any frame
  stronger than −75 dBm.
- **Virtual carrier sense (NAV)** — every frame we send declares a
  `durationUs` in its header. Any node overhearing it treats the medium as
  busy for that long. This is the same idea as the 802.11 RTS/CTS duration
  field, and it covers the hidden-terminal case that physical sensing misses.

**Q. Why does the backoff counter freeze instead of restarting?**
This is the single most important detail in the MAC. If both nodes redrew
after deferring, they would be equally likely to collide again. By freezing,
the node that already counted down most resumes closest to zero and so
transmits first — contention is resolved monotonically instead of being
re-randomised. It is what 802.11 does, and `backoffFreezes` in `mac` counts
how often it happened.

**Q. Explain binary exponential backoff.**
On every ACK timeout the contention window doubles (8 → 16 → 32 → 64 slots),
so retrying nodes spread further apart in time and the chance of a repeat
collision falls geometrically. A successful ACK resets it to the minimum.
Capping at 64 bounds worst-case latency.

**Q. What happens after the retry limit?**
After 4 retries the frame is dropped, counted in `txDropped`, and
`routeReportTxResult(peer, false)` is called — so a persistently failing link
immediately degrades that neighbour's route cost. MAC failure feeds routing
rather than being silently absorbed.

**Q. How do you acknowledge?**
The receiver of a unicast data-bearing frame queues an `FT_ACK` and sends it
after SIFS (300 µs) **without contending** — as 802.11 does, because an ACK
that had to contend could be beaten by a new transmission. Broadcasts are
never acknowledged; with four receivers you would get an ACK implosion.

**Q. Your channel never seems busy. Is CCA really working?**
`mac` reports `ccaBusyDeferrals`, which climbs in a normal run because all
four nodes contend in the CAP slot every superframe. If the lab band is quiet
and you want to see it move more, the forced-collision test guarantees it.
If the band were *saturated*, `forcedTx` would climb instead — that is the
counter for "medium busy longer than 50 ms, transmitted anyway", which exists
so a node can never be starved indefinitely.

---

## D · TDMA and scheduling (2 marks)

**Q. Why TDMA on top of CSMA/CA? Isn't that redundant?**
They solve different problems. TDMA gives deterministic, collision-free access
and — crucially — lets a node know exactly when it may sleep, which is where
the energy saving comes from. CSMA/CA handles the slots where contention is
unavoidable: the CAP slot, joins, route repair, and any residual overlap from
clock skew. Real WSN MACs such as IEEE 802.15.4 combine both in exactly this
way: a contention-free period plus a contention access period.

**Q. How are the nodes time-synchronised?**
N3's beacon carries its own clock in `netTimeMs`. Each node sets
`offset = beaconTime − localMillis()`, and `tdmaNetworkTimeMs()` returns
`millis() + offset`. Propagation over a few metres is well under a
millisecond, so it is ignored; the dominant error is receive-queue latency,
which is why the beacon is handled promptly. Guard bands of 20 ms at each slot
edge absorb the residual skew.

**Q. Why does end-to-end delay need that clock?**
Because the timestamp is written by one board and read by another. Without a
shared clock the subtraction would measure the difference between two
unrelated boot times, not the delay.

**Q. In what sense is the schedule dynamic?**
Slot 3 is unassigned by default — therefore a sleep slot. The cluster head
grants it when a node raises a sensor alarm or reports a backlog of 3+ frames,
and releases it when the condition clears. Separately, a node that misses
three consecutive slots has its slot revoked and gets it back when it returns.
Every change bumps `schedVersion` and carries a reason code that prints in
plain English. It tracks demand; it is not on a timer.

**Q. How do you know a stale beacon won't undo a schedule change?**
Nodes adopt a beacon only when its `schedVersion` differs from the one they
hold, and the head only ever increments. A late-overheard old beacon cannot
roll the network backwards.

**Q. Is your sleep real?**
Yes. `esp_wifi_stop()` powers down the Wi-Fi MAC and PHY — about 40 mA on an
ESP32 — and `esp_wifi_start()` brings it back before the next slot the node
needs. The CPU stays awake so the serial log keeps narrating, which is what
you want in a demo. `ENABLE_LIGHT_SLEEP` switches to `esp_light_sleep_start()`,
which sleeps the whole SoC to under a milliamp; `millis()` survives it because
`esp_timer` is RTC-backed. The duty cycle is reported in `sched`.

**Q. Why doesn't the sink sleep?**
It is mains-powered and must keep the dashboard reachable. This asymmetry is
standard in WSN design: energy is scarce at the leaves, not at the gateway.

**Q. What is "unused slot handling"?**
If a node owns a slot but has nothing queued when the slot ends, it logs the
slot as unused and the counter feeds the head's reclamation logic. The airtime
is not wasted on an empty transmission, and persistent silence eventually
costs the node its slot.

---

## E · Broadcast and unicast (1 mark)

**Q. What is broadcast and what is unicast, and why?**
Broadcast: `BEACON` (schedule), `ROUTE_UPDATE` (route control), `NET_STATUS`
(network status), `HELLO` (neighbour discovery), `CMD`. Unicast: `DATA`,
`AGG`, `LOC`, `SLOT_REQ`, `ACK`. The rule is reliability: anything that needs
a guaranteed, acknowledged delivery to one specific node is unicast; anything
every node needs and that will be repeated next superframe anyway is
broadcast. Acknowledging a broadcast to four receivers would cost four ACKs
for information that is refreshed every two seconds regardless.

**Q. How do you show delivery actually succeeded?**
Every transmission logs `TX … UNICAST N2->N3 (final N4 …)` and every success
logs a matching `ACK from N3 for seq=37 (31 ms, 1 attempt) - delivered`. The
sink additionally prints the full traversed path and counts per-source
delivery.

---

## F · Aggregation (2 marks)

**Q. What does your aggregation function compute?**
Per field (temperature, humidity, gas): minimum, maximum and mean, with a
per-field sample count. Plus the OR of all alarm bits, a bitmap of
contributing sources, and the byte/packet counters the reduction is computed
from.

**Q. Why not just the mean?**
Because a mean hides the event the network exists to detect. With readings of
1110, 212 and 212 ppm the mean is 661 — unremarkable — while the maximum,
1110, is the alarm. Sending min and max costs 8 extra bytes and preserves the
information that matters. The same reasoning is why alarm bits are OR-ed:
aggregation must never be able to swallow an alarm.

**Q. How is data reduction calculated?**
`reduction = (1 − aggregatedBytes / rawBytes) × 100`, accumulated over the
run, reported both byte-wise and packet-wise. Three 47-byte frames (141 B)
become one 75-byte frame: 46.8 % fewer bytes and 66.7 % fewer packets.

**Q. Byte reduction is only 47 % but packet reduction is 67 %. Which matters?**
Packet reduction, usually. Each transmission costs a backoff, a preamble, an
ACK and a receive window regardless of payload size, so per-packet overhead
dominates energy and airtime in a WSN. Byte reduction is the more conservative
number, which is why we report both.

**Q. What happens if one sensor fails mid-run?**
Its field's validity bit is cleared, and the aggregator excludes that field
from min/max/mean rather than averaging in a zero. Per-field sample counts are
what make this possible — a node with a dead humidity sensor still contributes
its temperature.

---

## G · Routing (2 marks)

**Q. What is your route metric?**
`cost = 100·(hops+1) + 150·(1−rssiNorm) + 200·(1−ackRate) + 100·lossRate`,
and infinite if the link is down. `rssiNorm` maps −95 dBm→0 and −35 dBm→1.
That is all five parameters the brief lists.

**Q. Why is ACK rate weighted highest?**
It is the most direct evidence that a link actually delivers. RSSI measures
received power, not success — a link can show −55 dBm and still fail because
of interference or a congested receiver. Hop count is the weakest signal of
the three, which is why it carries the smallest weight.

**Q. Where does the RSSI in that metric come from?**
Preferentially the Wi-Fi RSSI harvested by the MAC's promiscuous sniffer
(which parses `addr2` out of the 802.11 header, so it works identically on
Arduino core 2.x and 3.x despite their different ESP-NOW callbacks). If Wi-Fi
RSSI is not yet available it falls back to the BLE RSSI from the localization
phase. That fallback is one of the three concrete places where the BLE half of
this system feeds the Wi-Fi half.

**Q. All four boards are on one bench and can hear each other. Is your
multi-hop routing real?**
The routing is real; the adjacency is declared. `ROUTE_LINK_ALLOWED` in
`wsn_config.cpp` states the topology the deployment is meant to have — N1 has
no direct link to the sink — because otherwise every node would reach the sink
in one hop and multi-hop, aggregation and recovery would never execute on a
bench. Everything downstream of that adjacency is genuine: the cost
computation, the ACK-based failure detection, the alternate-route selection,
the relaying through N2, the recovery. Spacing the nodes down a corridor gives
the same topology physically, and setting the matrix to all 1s lets the metric
discover it unaided. We state it in the report rather than hiding it.

**Q. Why does N1 prefer N3 when N2 would also do?**
A −150 cost discount for the cluster head. A sensor node reporting to its head
is what makes aggregation possible; going around the head would deliver the
same readings in three times as many packets. It is a discount rather than a
rule, so when N3's link fails the arithmetic lets N2 win on its own merits —
which is exactly what you see in the recovery demonstration.

**Q. You have two failure detectors. Why?**
They catch different failures. The ACK-loss detector catches a peer we are
actively transmitting to. The 7-second silence timeout catches a neighbour
that was powered off and that we were never sending to in the first place — it
would never fail an ACK, so the first detector would never fire.

**Q. How do you prevent routing loops?**
Three ways: a TTL that bounds any loop to 5 hops; a `path[]` list in the
header so a forwarder refuses to hand a frame to a node already in its path;
and hop counts advertised in `HELLO`, so a neighbour whose own route is
through us does not look attractive.

**Q. After recovery, why doesn't the route snap straight back?**
Because snapping back would be wrong if the original link were still marginal.
Hearing N3 again only clears the down flag; the original path then has to win
on cost against the current alternative. It normally does, within a second,
because it has better RSSI and a clean ACK history — but it wins on evidence.

**Q. What is the failover latency?**
Measured from detection to alternate-route selection and reported as
`failoverMs` — typically 150–250 ms, dominated by the three ACK timeouts (60 ms
each) that constitute detection.

---

## H · Performance (2 marks)

**Q. How is PDR computed?**
At the sink, per source: `received / expected`, where `expected` is the
observed sequence span `lastSeq − firstSeq + 1`. That is the honest definition
at a receiver — it counts gaps, so a packet lost in transit is visible even
though the sink never heard about it. Duplicates are detected and discarded
first, otherwise PDR could exceed 100 %.

**Q. Your mean delay is a few hundred milliseconds. Isn't that slow?**
It is dominated by the TDMA structure, not the radio. A reading taken just
after a node's slot waits almost a full 2-second superframe for the next one,
and the aggregate then waits for slot 4. One-hop MAC latency is only about
30 ms — you can see that in the ACK RTT. The delay is the price we deliberately
pay for a 46 % duty cycle; shortening the superframe trades energy for latency,
and that trade is a one-line change to `TDMA_SLOT_MS`.

**Q. Throughput looks very low.**
It is, and appropriately so. A WSN is not a throughput network: four nodes
each sending ~24 bytes of payload every two seconds is around 100 bit/s by
design. The figure is useful as a *relative* measure — it drops visibly during
a route failure and recovers afterwards.

**Q. How do you count retransmissions network-wide?**
Each node keeps a cumulative MAC retry counter and carries it in the
`retxTotal` field of every data frame. The sink sums the latest value per
source, so it has a network-wide figure without needing a separate reporting
protocol.

**Q. Which four metrics would you present if asked for only four?**
PDR, end-to-end delay, data reduction percentage and localization error. They
cover reliability, latency, the aggregation benefit and the BLE subsystem —
one from each part of the system, rather than four that all measure
reliability.

---

## I · Awkward questions, answered honestly

**Q. Is your ppm figure trustworthy?**
No, and the report says so. The MQ heaters run at 3.3 V rather than the rated
5 V to keep their analog output inside the ESP32's input range without extra
components, so the sensing element is below its intended temperature and the
datasheet curve does not strictly apply. The sensors have also had no 24–48 h
burn-in. `R0` is calibrated on this hardware at this heater voltage, so the
`Rs/R0` ratio is self-consistent and the *relative* response is reliable and
repeatable — which is what the alarm logic and the demonstration depend on.
Accurate absolute ppm needs 5 V plus a 10 kΩ/10 kΩ divider and a one-line
change to `MQ_DIVIDER_GAIN`.

**Q. Isn't the collision demo staged?**
Yes, deliberately — the brief asks us to *show* two nodes transmitting at
approximately the same time, and waiting for one to happen by chance is not a
demonstration. But what we stage is only the *timing*: the cluster head grants
both nodes the same slot. Everything after that is the real MAC — real carrier
sense, real random draws, a real freeze, a real ACK. `collide hard` additionally
bypasses carrier sense so a genuine collision occurs and the timeout path runs.
Contention also happens unstaged in every CAP slot, which is why
`backoffFreezes` is non-zero before we touch anything.

**Q. Why only three anchors?**
That is all four nodes allow, with one of them being the node to localize.
Three is the minimum for a 2-D fix, so there is no redundancy and no way to
detect a bad range measurement. A fifth board would let us drop the worst
residual.

**Q. What is the weakest part of this system?**
RSSI-based ranging. Everything downstream of it — the position, the error, the
RMSE — inherits its noise, and no amount of filtering fixes an amplitude-based
distance estimate in a multipath environment. Second weakest is time
synchronisation: a single-beacon, one-way offset with no round-trip correction,
which is adequate at 20 ms guard bands but would not scale to shorter slots.

**Q. What would you do differently with more time?**
Add RTS/CTS for the hidden-terminal case rather than relying on NAV from
overheard data frames; replace the single global path-loss exponent with a
per-link calibration; and add a second aggregation tier so that N2, when
relaying, also aggregates rather than forwarding each frame individually.
