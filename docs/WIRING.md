# Circuit and Network Setup

**23CSE341 WSN — Lab Evaluation 2** · deliverable: *Circuit/network setup*

---

## 1. Read this first — two rules that will cost you a board

### Rule 1 — The knob on the back of the MQ module is **not** a voltage divider

The small blue potentiometer on the MQ2 and MQ135 breakouts feeds the **LM393
comparator** that drives the **digital `DOUT`** pin. It sets the digital
trip point and nothing else. It has **no effect whatsoever on the analog
`AOUT`** line, which still swings all the way up to whatever you fed `VCC`.

If `VCC` is 5 V, `AOUT` reaches ~5 V, and an ESP32 GPIO is rated for 3.3 V.

**In this build both MQ modules run from 3V3**, so `AOUT` can never exceed
3.3 V and no divider is fitted. See §5 for what that trade costs.

### Rule 2 — Both MQ analog pins must be on **ADC1** (GPIO 32–39)

ADC2 (GPIO 0, 2, 4, 12–15, 25, 26, 27) is owned by the Wi-Fi driver. While
Wi-Fi is running — which, in this project, is always — an ADC2 read returns
garbage with no error. If either MQ sensor is currently on an ADC2 pin, it
**must** be moved. This is a silicon limitation, not something firmware can
work around.

---

## 2. Node, sensor and role allocation

| Node | Role | Sensor | Why this pairing |
|---|---|---|---|
| **N1** | Sensor | DHT22 | Pure data source, and the *mobile* node whose position is estimated |
| **N2** | Sensor / Relay | MQ2 (smoke/LPG) | Its gas alarm is one of two events that trigger a dynamic schedule change; it also relays for N1 after a failure |
| **N3** | Cluster head / Aggregator / Router | MQ135 (air quality) | Aggregates three sources — N1, N2 and its own reading |
| **N4** | Sink / Gateway | HC-SR501 (PIR) | Motion at the gateway is the second schedule-change trigger; hosts the dashboard |

---

## 3. Pin map

Every pin is a `#define` at the top of `common/wsn_config.h`. If your wiring
differs, change it there rather than rewiring.

### Node 1 — DHT22

| Sensor pin | ESP32 | Notes |
|---|---|---|
| `VCC` | **3V3** | |
| `DATA` | **GPIO 4** | **10 kΩ pull-up from DATA to 3V3 is required** |
| `GND` | GND | |
| — | GPIO 2 | On-board LED: lights during this node's TDMA slot |

```
   DHT22                    ESP32
  +-------+                +-------+
  | VCC   |----------------| 3V3   |
  |       |        +--[10k]--+
  | DATA  |--------+--------| GPIO4 |
  | GND   |----------------| GND   |
  +-------+                +-------+
```

> The DHT22's data line is open-drain and will not produce a clean logic high
> without that pull-up. Many breakout boards already have one fitted — check
> for a resistor next to the connector before adding a second.

### Node 2 — MQ2 (smoke / LPG)

| Sensor pin | ESP32 | Notes |
|---|---|---|
| `VCC` | **3V3** | **Not 5 V.** See Rule 1. |
| `AOUT` | **GPIO 34** | ADC1_CH6, input-only pin |
| `DOUT` | GPIO 18 *(optional)* | Comparator output; firmware uses it only as a cross-check |
| `GND` | GND | |

### Node 3 — MQ135 (air quality)

| Sensor pin | ESP32 | Notes |
|---|---|---|
| `VCC` | **3V3** | **Not 5 V.** See Rule 1. |
| `AOUT` | **GPIO 35** | ADC1_CH7, input-only pin |
| `DOUT` | GPIO 18 *(optional)* | |
| `GND` | GND | |

```
   MQ2 / MQ135              ESP32
  +---------+              +--------+
  | VCC     |--------------| 3V3    |
  | AOUT    |--------------| GPIO34 |   (N2)
  |         |              | GPIO35 |   (N3)
  | DOUT    |- - optional -| GPIO18 |
  | GND     |--------------| GND    |
  +---------+              +--------+
       ^
       '-- the blue trimpot sets the DOUT threshold only.
           It does NOT attenuate AOUT.
```

### Node 4 — HC-SR501 (PIR)

| Sensor pin | ESP32 | Notes |
|---|---|---|
| `VCC` | **5V / VIN** | The HC-SR501 needs 4.5–20 V; it will not work on 3V3 |
| `OUT` | **GPIO 27** | 3.3 V TTL output — safe to connect directly |
| `GND` | GND | |

Two settings on the module matter for the demonstration:

- **Trigger jumper → `H`** (repeatable). In `L` mode the output will not
  re-trigger while already high, which makes motion look intermittent.
- **Time-delay trimmer → fully anticlockwise** (~3 s). The factory setting can
  be 30 s or more, which makes the alarm look stuck on.
- The HC-SR501 needs ~60 s after power-up to settle. The firmware's 120 s
  warm-up window covers this; ignore spurious triggers before it ends.

---

## 4. Power

| Node | Draw (approx.) | Supply |
|---|---|---|
| N1 | ESP32 ~160 mA peak + DHT22 ~2 mA | USB |
| N2 | ESP32 ~160 mA peak + MQ2 heater ~80 mA @ 3V3 | USB |
| N3 | ESP32 ~160 mA peak + MQ135 heater ~80 mA @ 3V3 | USB |
| N4 | ESP32 ~200 mA peak (SoftAP) + PIR ~1 mA | USB |

- **Every node needs its own 5 V source.** Four USB ports, a powered hub, or
  wall adapters.
- A laptop USB port that sags under load causes brownout resets during Wi-Fi
  transmit bursts. Those show up in your results as *packet loss* and will
  quietly ruin the PDR measurement. If you see
  `Brownout detector was triggered` on the serial console, move that node to a
  better supply before collecting any data.
- **All grounds must be common** — this is already true if every board shares
  the same hub, but verify it if you split supplies.

---

## 5. What running the MQ sensors at 3.3 V costs

The MQ heater is specified for 5.0 V. At 3.3 V the sensing element sits below
its intended operating temperature, so:

- **Sensitivity is reduced** — the resistance swing per unit of gas is smaller.
- **Absolute ppm is not trustworthy.** The datasheet curve the firmware uses
  (`ppm = a · (Rs/R0)^b`) is characterised at the rated heater voltage.
- **Relative response is still clear and repeatable** — which is what the
  demonstration actually needs: a lighter held near the MQ2, or breath on the
  MQ135, produces an unmistakable step.

This is handled honestly rather than hidden:

- `R0` is calibrated in clean air *on this hardware, at this heater voltage*,
  and stored in NVS, so the ratio `Rs/R0` is self-consistent.
- The report presents the gas figures as an **indexed/relative concentration**,
  states the heater voltage, and says why.

**If you later want accurate ppm:** move both MQ modules to 5 V and fit a
10 kΩ + 10 kΩ divider on each `AOUT` line, then set
`#define MQ_DIVIDER_GAIN 2.0f` in `wsn_config.h`. Nothing else changes.

```
            with a 5 V supply ONLY:
  MQ AOUT --[ 10k ]--+-- GPIO34 / GPIO35
                     |
                  [ 10k ]
                     |
                    GND
```

A second caveat worth stating in the viva: these sensors have had no 24–48 h
burn-in, so the baseline continues to drift slowly for the first day or so of
use. Re-running `calib` just before the demonstration gives the best results.

---

## 6. Network setup

No router, no access point, no Wi-Fi credentials. The nodes talk over
**ESP-NOW**, a connectionless Wi-Fi protocol built on 802.11 vendor action
frames, and the sink creates its own AP purely for the dashboard. This removes
the single biggest demo-day failure mode: campus Wi-Fi.

| Item | Value |
|---|---|
| Protocol | ESP-NOW (802.11 action frames), STA mode |
| Channel | **1** — shared by ESP-NOW *and* the sink's SoftAP |
| Addressing | 6-byte MAC per peer + a logical node id 1–4 in every frame header |
| Broadcast | `FF:FF:FF:FF:FF:FF` |
| Encryption | Off (keeps the frames readable in a sniffer during the viva) |
| Dashboard | SSID `WSN_SINK`, password `wsnlab2024`, `http://192.168.4.1` |

> **The SoftAP must be on the ESP-NOW channel.** One radio serves both. If
> they disagree, the AP still looks fine and ESP-NOW silently stops receiving.
> Both are pinned to channel 1 in `wsn_config.h`.

### Topology

```
                 BLE (localization)              Wi-Fi / ESP-NOW (data)
        anchors: N2, N3, N4                PRIMARY   N1 ─▶ N3 ─▶ N4
        mobile : N1                                  N2 ─▶ N3 ─▶ N4
                                           ALTERNATE N1 ─▶ N2 ─▶ N4
                                           FALLBACK  N1 ──────▶ N4
```

### Physical placement

Lay out a coordinate frame with a tape measure and enter it in
`common/wsn_config.cpp`. The defaults describe a 4.0 m × 3.0 m area:

```
   y
   ^
 3 +  N4 (0, 3)  ...................... sink / anchor
   |
   |
   |              N1 (2.0, 1.5)  <-- mobile node, ground truth
   |
 0 +  N2 (0, 0) ................. N3 (4, 0)
   +----------------------------------------> x
   0                              4
```

Two things matter more than the exact distances:

1. **The anchors must not be collinear.** Three anchors on one line cannot fix
   a 2-D position; the solver detects this and says so rather than returning
   nonsense. Keep them roughly triangular.
2. **The coordinates you type must match reality.** Localization error is
   measured against them, and it is worth 3 marks.

Use `settrue <x> <y>` on N1's console to update its ground truth when you move
it, so you can tabulate the error at several positions.

---

## 7. Pre-flight checklist

- [ ] MQ2 `VCC` on **3V3** (not 5 V) — verify with a multimeter
- [ ] MQ135 `VCC` on **3V3** (not 5 V)
- [ ] MQ2 `AOUT` on GPIO 32–39; MQ135 `AOUT` on GPIO 32–39
- [ ] DHT22 has a 10 kΩ pull-up to 3V3 on its data line
- [ ] HC-SR501 on 5V/VIN, trigger jumper on `H`, delay trimmer at minimum
- [ ] All grounds common
- [ ] Each node on a supply that holds up under load
- [ ] All four MAC addresses pasted into `common/wsn_config.cpp`
- [ ] Anchor coordinates in `wsn_config.cpp` match the tape measure
- [ ] Partition scheme set to **Huge APP** on every board
- [ ] `python tools/sync_common.py` run after any edit to `common/`
- [ ] MQ sensors powered for ≥2 minutes before trusting a reading
- [ ] `calib` run on N2 and N3 in clean air
