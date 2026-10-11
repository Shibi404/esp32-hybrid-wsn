# Bring-Up and Testing Guide

**Read this before the first power-on.** `DEMO_SCRIPT.md` is for the examiner;
this is for getting the thing working in the first place.

The rule throughout: **add one node at a time.** If you flash all four and
nothing happens, you have four possible causes and no way to separate them. At
every stage below there is exactly one new variable.

Budget about 90 minutes for a first bring-up.

---

## Stage 0 — Before you apply power (5 min)

This is the only stage that can destroy hardware. Do not skip it.

| Check | How | Why |
|---|---|---|
| **MQ2 `VCC` is on 3V3, not 5V** | Multimeter between the module's VCC pin and GND, board powered from USB with the MQ `AOUT` wire **disconnected** | At 5 V the analog output reaches ~5 V and will damage a 3.3 V GPIO. The blue trimpot does *not* protect it — it only sets the digital `DOUT` threshold. |
| **MQ135 `VCC` is on 3V3** | Same | Same |
| **MQ2 `AOUT` → GPIO 32–39** | Visual | ADC2 is owned by the Wi-Fi driver; a read there returns garbage with no error |
| **MQ135 `AOUT` → GPIO 32–39** | Visual | Same |
| **DHT22 has a 10 kΩ pull-up** to 3V3 on its data line | Visual — many breakouts have one fitted already | Open-drain line will not produce a clean logic high without it |
| **PIR on 5V/VIN** | Visual | HC-SR501 needs 4.5–20 V; it will not work on 3V3 |
| **All grounds common** | Continuity test | |

Also set on the PIR module now: **trigger jumper → `H`** (repeatable) and the
**time-delay trimmer fully anticlockwise** (~3 s).

> ✅ **Pass:** every MQ `VCC` measures ~3.3 V and both `AOUT` wires land on a
> pin numbered 32–39.

---

## Stage 1 — Toolchain and MAC addresses (10 min)

### 1.1 Confirm the build works at all

```bash
python tools/build_all.py
```

> ✅ **Pass:** four `ok` lines and a flash/RAM table.
>
> ❌ `arduino-cli not found` → compile from the IDE instead; everything else
> in this guide still applies.
> ❌ `text section exceeds available space` → Partition Scheme is not set to
> **Huge APP**.

### 1.2 Collect the MAC addresses

Flash `tools/print_mac/print_mac.ino` to each board in turn. Serial Monitor at
**115200**.

Label each board physically as you go — masking tape and a marker. You will
regret not doing this.

Paste all four into the `NODE_MAC` table in **`common/wsn_config.cpp`**, then:

```bash
python tools/sync_common.py
```

> ⚠️ Use the **STA MAC** the sketch prints, not the SoftAP MAC. They differ by
> one bit and ESP-NOW silently discards frames sent to the wrong one.

---

## Stage 2 — One node at a time: sensors only (15 min)

Flash each node its own sketch and check *only* the sensor. Networking comes
later.

| Board | Sketch |
|---|---|
| DHT22 board | `firmware/Node1_Sensor` |
| MQ2 board | `firmware/Node2_SensorRelay` |
| MQ135 board | `firmware/Node3_ClusterHead` |
| PIR board | `firmware/Node4_Sink` |

Power each one **alone**, open Serial Monitor at 115200, and check the boot
banner:

```
  Node        : N2 (id 2)
  Role        : SENSOR/RELAY
  STA MAC     : 24:6F:28:AA:BB:CC
  Channel     : 1      Superframe: 2000 ms / 8 slots
```

> ❌ **`*** WARNING: this board's MAC is NOT in the NODE_MAC table ***`**
> → Stage 1.2 was not done, or not re-synced. Nothing will communicate.
>
> ❌ **`*** WARNING: NODE_MAC says this board is N3, but this sketch is N2 ***`**
> → you flashed the wrong sketch to this board. Swap them.

Now wait **2 minutes** for warm-up and type:

```
sensors
```

| Node | Expect | If wrong |
|---|---|---|
| N1 | `Temperature: 27.40`, `Humidity: 58.20` | `INVALID` → missing 10 kΩ pull-up, or wrong GPIO |
| N2 | `ADC raw: 1840`, `R0 (clean air): 4.82`, a ppm value | `raw: 0` → not connected / wrong pin. `raw: 4095` → saturated or still on 5 V. `NOT CALIBRATED` → run `calib` |
| N3 | same as N2 | same |
| N4 | `Motion: none`, then `DETECTED` when you wave | always `DETECTED` → trigger jumper on `L`, or delay trimmer too high |

Then provoke each sensor and watch the value move:

- **N2 (MQ2):** hold an **unlit** lighter near it and press the gas button for
  2–3 s. ppm should climb sharply within a few seconds.
- **N3 (MQ135):** breathe on it from ~5 cm. ppm should rise.
- **N1 (DHT22):** cup it in your hand; humidity rises first, then temperature.
- **N4 (PIR):** wave; `PIR motion event #1` appears.

> ✅ **Pass:** all four sensors produce plausible values that respond to
> stimulus. Do not proceed until this is true — a sensor fault later will look
> like a network fault.

---

## Stage 3 — N3 + N4: the network skeleton (10 min)

Power **only N3 and N4**. N3 is the time master; nothing else can transmit
until it is running.

### On N3

```
sched
```

> ✅ `Synced: yes`, schedule v1, slot map printed, superframe counter rising.

You will also see, repeatedly:

```
[AGG ][I] Aggregated 1 readings from 1 source(s) ...
[NODE][W] No route to the sink - aggregate held
```

That second line is **correct** at this moment if N4 has not been heard yet.
It should disappear within a few seconds of N4 booting.

### On N4

```
status
```

> ✅ `Phase: WI-FI NETWORK OPERATION`, and a `Dashboard: http://192.168.4.1`
> line.

Within ~4 s of N3 running you should see N4 adopt the schedule:

```
[TDMA][I] Time synchronised to N3 (offset +1423 ms)
```

> ❌ **N4 never syncs** → N3 is not beaconing (check `sched` on N3), or the MAC
> table is wrong, or the two boards are on different channels (both are pinned
> to 1 in `wsn_config.h`, so this means one was flashed from a stale copy —
> re-run `sync_common.py` and re-flash).

### Dashboard

On a phone or laptop, join Wi-Fi **`WSN_SINK`** / **`wsnlab2024`**, then open
**<http://192.168.4.1>**.

> ✅ The page loads, N3 and N4 show green, and the aggregation panel starts
> filling.
>
> ❌ **AP visible but page will not load** → you are probably still on mobile
> data; disable it. Try `http://192.168.4.1` explicitly (not `https`).
> ❌ **AP not visible** → the sink's `onWifiUp` failed; check its serial for
> `SoftAP failed to start`.

> ✅ **Stage 3 pass:** N4 syncs to N3, the dashboard loads, and N3's "no route"
> message has stopped.

---

## Stage 4 — Add N2: real aggregation (5 min)

Power N2 as well.

On **N2**, within ~4 s:

```
[TDMA][I] Time synchronised to N3 (offset ...)
[MAC ][I] CCA idle for DIFS -> backoff = 4 slots (CW=8, slot=500 us)
[MAC ][I] TX DATA    seq=1 UNICAST N2->N3 (final N4, hop 0, 47 B, path 2)
[MAC ][I] ACK  from N3 for seq=1 (31 ms, 1 attempt) - delivered
```

On **N3**:

```
[AGG ][I] Aggregated 2 readings from 2 source(s) [mask 0x06]: 2 packets / 94 B
          -> 1 packet / 75 B  (20.2 % fewer bytes)
```

> ✅ **Pass:** `2 source(s)`, and the sink's dashboard shows N2 online with a
> `viaAgg` count climbing.
>
> ❌ **N2 transmits but never gets an ACK** → N3's MAC entry for N2 is wrong,
> or N2 is not in N3's `NODE_MAC`. Re-check Stage 1.2.
> ❌ **N2 never transmits** → it is not synced; check `sched` on N2 for
> `Synced: NO`.

---

## Stage 5 — Add N1: full network and localization (10 min)

Power N1. Place the four boards at the positions you entered in
`wsn_config.cpp` (defaults assume a 4.0 m × 3.0 m rectangle — see
`docs/WIRING.md` §6).

On **N1** you will first see a BLE scan:

```
[BLE ][I] Scanning for 3 s ...
[BLE ][I] N2: RSSI med=-61 dBm (n=11, ewma=-60.8)  d_est=1.86 m ...
[BLE ][I] N3: RSSI med=-68 dBm (n=9,  ewma=-67.4)  d_est=3.21 m ...
[BLE ][I] N4: RSSI med=-64 dBm (n=12, ewma=-64.1)  d_est=2.37 m ...
[BLE ][I] Nearest node: N2 at -61 dBm (~1.86 m)  [proximity result]
[BLE ][I] LOCALIZATION: estimated (2.31, 1.22) m | true (2.00, 1.50) m
          | error 0.42 m | ...
```

> ❌ **`No WSN neighbours discovered`** → the other three are not advertising.
> They advertise from boot and never stop, so this means they are not powered
> or are out of BLE range (shorten the distances).
>
> ❌ **`Localization needs 3 anchors, heard 2`** → one anchor is not being
> heard. Usually N4; check `SINK_BLE_ANCHOR` is `1` in `wsn_config.h`.
>
> ❌ **Error is several metres** → expected before calibration. Stage 6 fixes
> it.

Then N3 should report three sources:

```
[AGG ][I] Aggregated 3 readings from 3 source(s) [mask 0x07]: 3 packets / 141 B
          -> 1 packet / 75 B  (46.8 % fewer bytes)
```

And on **N4**:

```
perf
```

> ✅ **Pass:** PDR above 90 %, a non-zero delay, data reduction near 47 %, and
> the dashboard showing all four nodes green with a `1→3→4` path.

### If PDR is poor (< 85 %)

Check for `Brownout detector was triggered` on any node's serial. A USB port
that sags under Wi-Fi transmit bursts resets the board, and that shows up in
your results as packet loss. Move the node to a powered hub or wall adapter
before collecting any data — every number you record depends on this.

---

## Stage 6 — Calibration (10 min)

Do this once, in the room you will demonstrate in, shortly before you collect
results.

### 6.1 Gas sensors

With N2 and N3 in clean air (no breath, no lighter, windows open if possible):

```
calib          ← on N2, then on N3
```

> ✅ `Calibration OK: Rs(clean)=48.21 kOhm, R0=4.90 kOhm (saved to NVS)`
>
> ❌ `only 3/25 valid samples` → wiring fault; go back to Stage 2.

Stored in flash, so it survives power cycles. Re-run it if you move rooms.

### 6.2 BLE ranging

Hold N1 **exactly 1.00 m** from N3 (tape measure, nothing in between), then on
N1:

```
calib1m 3
```

> ✅ `Calibrated RSSI@1m = -58.0 dBm from 11 samples (saved to NVS)`

Then set N1's true position and watch the error:

```
settrue 2.0 1.5
loc
```

If the error is still large, tune the path-loss exponent. Lower `n` makes
estimated distances **larger**; higher `n` makes them **smaller**:

```
pathloss 2.2      ← try 2.0 to 3.0; 2.5 is the default
loc               ← check after the next BLE round (~30 s)
```

> ✅ **Pass:** error consistently under ~1 m. Typical indoor results are
> 0.3–1.0 m; below 0.3 m would be suspicious for RSSI ranging.

Optionally repeat at three positions and record each — `docs/RESULTS.md` §6 has
the table.

---

## Stage 7 — Test each graded feature (20 min)

Now verify the things the rubric actually scores. Each is one command.

| # | Test | Command | Pass criterion |
|---|---|---|---|
| 1 | Contention handling | `collide` | Two nodes draw **different** backoffs; one logs `backoff FROZEN at N`; both deliver |
| 2 | Forced collision recovery | `collide hard` | `ACK TIMEOUT` → `CW 8 -> 16` → retry → delivered |
| 3 | Dynamic schedule | Hold a lighter near N2's MQ2 | N3 logs `SCHEDULE UPDATED to v2: alarm raised - dynamic slot granted (N2)`; slot 3 changes from SLEEP to DATA |
| 4 | Schedule release | Let the alarm clear | `v3: alarm cleared - dynamic slot released` |
| 5 | Unused slots + sleep | `sched` on N1 | `Slot N was mine but UNUSED`, and a duty cycle under 60 % |
| 6 | Aggregation | `agg` on N3 | 3 sources, ~47 % byte / ~67 % packet reduction |
| 7 | **Route failure** | `fail 3` | N1/N2 log ACK timeouts → `LINK FAILURE DETECTED` → `ALTERNATE ROUTE SELECTED: N3 -> N2` → sink receives on `1→2→4` |
| 8 | **Route recovery** | `recover 3` | `Link to N3 RECOVERED` → `ROUTE CHANGED: N2 -> N3 [primary path]` |
| 9 | Physical failure | Unplug N3 for 10 s | Same as #7, but detected by the 7 s silence timeout instead |
| 10 | Metrics | `perf` on N4 | All seven metrics populated |

For #7, watch **two** serial windows side by side (N1 and N4) — the interesting
sequence lasts about two seconds.

> ⚠️ After any `fail`, always `recover` before moving on. A node left playing
> dead will quietly skew everything that follows.

---

## Stage 8 — Capture a clean run for the report (20 min)

Everything above was debugging. Now do one continuous, undisturbed run and
record it.

```bash
python tools/serial_logger.py --port COM7      # the SINK's port
```

Leave it running for **5–10 minutes**. During that window, in order:

1. Let it settle for 2 minutes (clean baseline PDR)
2. `collide`, then `collide hard`
3. Trigger the gas alarm, let it clear
4. `fail 3`, wait ~30 s, `recover 3`
5. Let it settle another 2 minutes

Ctrl-C, then:

```bash
python tools/plot_metrics.py
```

> ✅ Six PNGs and a `summary.md` in `results/run_<timestamp>/`.

Then:

- Copy the numbers from `summary.md` into `docs/RESULTS.md`
- Take the twelve screenshots in `docs/SCREENSHOT_CHECKLIST.md` **from this
  same run**, so every figure is mutually consistent
- Regenerate the report: `python tools/make_report_docx.py`
- Commit: `git add -A && git commit -m "results: capture run of <date>"`

---

## Stage 9 — Rehearse (15 min)

Run through `docs/DEMO_SCRIPT.md` once end to end, out loud, with the
equipment in its final position. Then read `docs/VIVA_QA.md` §I — the awkward
questions — because those are the ones that separate marks.

---

## Known trouble spots

| Symptom | Most likely cause | Fix |
|---|---|---|
| Nothing communicates at all | `NODE_MAC` table wrong | Stage 1.2; the boot banner warns you |
| One node never transmits | Not time-synced | `sched` → `Synced: NO`; check N3 is alive |
| ESP-NOW reception seems flaky | Promiscuous-mode CCA conflicting on your boards | Set `ENABLE_PHY_CCA 0` in `wsn_config.h`, re-sync, re-flash. The MAC falls back to NAV-only carrier sense; everything else is unaffected |
| Random resets, poor PDR | Brownout under Wi-Fi bursts | Powered hub or wall adapter |
| Serial prints gibberish | Wrong baud | 115200 |
| Gas reads `n/a` | `R0` never measured | `calib` in clean air |
| Localization error > 2 m | Ranging not calibrated for this room | `calib1m <id>`, then tune `pathloss` |
| Dashboard loads then freezes | You left a `fail` command in force on the sink | `recover 4` |
| A node lost its TDMA slot | It was silent for 4 superframes | It restores automatically on the next transmission |

Fuller table in `docs/DEMO_SCRIPT.md`.

---

## Quick reference

```
help      every command            calib          measure MQ R0 in clean air
status    phase, uptime, heap      calib1m <id>   BLE RSSI at exactly 1 m
stats     everything at once       pathloss <n>   tune the ranging model
sensors   readings + health        settrue <x> <y>  set N1's true position
loc       localization result      fail <id>      make a node play dead
mac       CSMA/CA counters         recover <id>   bring it back
sched     slot map + duty cycle    collide        contention test
nbr       neighbour table          collide hard   forced collision
agg       aggregation window       reset          clear all counters
perf      the seven metrics        debug on       verbose logging
```
