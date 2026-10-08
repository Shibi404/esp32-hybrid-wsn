# ESP32 Hybrid Wireless Sensor Network

**23CSE341 Wireless Sensor Networks — Lab Evaluation 2**

A 4-node hybrid **BLE + Wi-Fi** Wireless Sensor Network built on ESP32, integrating BLE
localization, a hand-written CSMA/CA MAC, dynamic TDMA scheduling, data aggregation,
adaptive multi-hop routing with route recovery, and a real-time web dashboard.

Every protocol mechanism is implemented at the application layer so its internals
(backoff counters, NAV timers, slot maps, route cost tables) are observable on the serial
console and the dashboard — rather than being hidden inside a vendor library.

---

## 1. At a glance

| Node | Role | Sensor | Key functions |
|------|------|--------|---------------|
| **N1** | Sensor node | DHT22 (temperature, humidity) | Sensing, BLE mobile node (self-localizes), originates data, CSMA/CA, TDMA slot 1 |
| **N2** | Sensor / Relay | MQ2 (smoke / LPG) | Sensing, BLE anchor, originates data, **relays N1 when the primary route fails** |
| **N3** | Cluster head / Aggregator / Router | MQ135 (air quality) | TDMA beacon + time master, data aggregation, routing, forwarding |
| **N4** | Sink / Gateway | HC-SR501 (PIR motion) | Data collection, performance metrics, SoftAP + real-time dashboard |

```
              BLE PHASE                                 WI-FI PHASE (ESP-NOW, ch 1)
   ┌──────────────────────────────┐          ┌────────────────────────────────────────┐
   │  anchors: N2, N3, N4          │          │  PRIMARY   N1 ─▶ N3 ─▶ N4   (2 hops)   │
   │  mobile : N1 → trilateration  │   ───▶   │            N2 ─▶ N3 ─▶ N4   (2 hops)   │
   │  RSSI → distance → (x,y)      │          │  ALTERNATE N1 ─▶ N2 ─▶ N4   (N3 down)  │
   │  → localization error         │          │  FALLBACK  N1 ──────▶ N4    (1 hop)    │
   └──────────────────────────────┘          └────────────────────────────────────────┘
```

---

## 2. Why BLE and Wi-Fi are time-phased

The ESP32 has a **single 2.4 GHz radio**. Running the BLE stack and ESP-NOW concurrently
starves the BLE scan window and destabilises RSSI — which is exactly the quantity this
experiment measures. The firmware therefore alternates two phases, mirroring the
demonstration flow required by the lab guidelines:

```
PHASE A — BLE  (~6 s)     advertise + scan → RSSI → distance → trilateration
                          → nearest node → localization error → BLEDevice::deinit()
                                   ↓
PHASE B — WI-FI (~30 s)   beacon/schedule → CSMA/CA → TDMA transmission
                          → aggregation → multi-hop → sink → metrics → sleep slots
                                   ↓
                              (loops forever)
```

`BLEDevice::deinit(true)` frees roughly 60 KB of RAM before Phase B, which is what allows
the sink to run BLE **and** a SoftAP **and** a web server on one chip.

---

## 3. Repository layout

```
esp32-hybrid-wsn/
├── common/                     Single source of truth for all shared firmware modules
│   ├── wsn_config.h/.cpp       Node IDs, MAC table, anchor coordinates, pins, timings
│   ├── wsn_protocol.h/.cpp     Wire format: packed frame header, payloads, CRC-16
│   ├── wsn_log.h/.cpp          Tagged human output + machine-readable CSV stream
│   ├── wsn_sensors.h/.cpp      DHT22 / MQ2 / MQ135 / PIR, warm-up, calibration, faults
│   ├── wsn_ble.h/.cpp          Advertising, scanning, RSSI filtering, trilateration
│   ├── wsn_mac.h/.cpp          CSMA/CA: CCA, NAV, DIFS, backoff, ACK, retransmission
│   ├── wsn_tdma.h/.cpp         Superframe, dynamic slot map, time sync, sleep/wake
│   ├── wsn_route.h/.cpp        Neighbor table, cost metric, failure detection, recovery
│   ├── wsn_metrics.h/.cpp      PDR, loss, delay, throughput, retransmissions, reduction
│   └── wsn_cli.h/.cpp          Serial command interface shared by all nodes
├── firmware/
│   ├── Node1_Sensor/
│   ├── Node2_SensorRelay/
│   ├── Node3_ClusterHead/
│   └── Node4_Sink/             (+ dashboard_html.h)
├── tools/
│   ├── print_mac/              Run once per board to read its Wi-Fi MAC
│   ├── sync_common.py          Copies common/ into each sketch folder
│   ├── serial_logger.py        Sink serial → results/metrics.csv
│   └── plot_metrics.py         CSV → PNG graphs for the report
├── docs/
│   ├── WIRING.md               Circuit setup, pin tables, power, safety notes
│   ├── ARCHITECTURE.md         Architecture diagram + protocol stack
│   ├── FLOWCHART.md            Per-node and per-protocol flowcharts
│   ├── DEMO_SCRIPT.md          Minute-by-minute demonstration script
│   ├── VIVA_QA.md              Likely viva questions with answers tied to the code
│   ├── RESULTS.md              Performance results template
│   ├── TECHNICAL_REPORT.md     Short technical report
│   └── SCREENSHOT_CHECKLIST.md Exactly which screenshots to capture
└── results/                    CSV logs and generated graphs
```

Arduino requires every source file to sit next to its `.ino`. `tools/sync_common.py`
copies `common/` into the four sketch folders, so **you only ever edit `common/`** and the
four nodes stay identical by construction.

---

## 4. Setup

### 4.1 Prerequisites

- **Arduino IDE 2.x**
- **esp32 board package by Espressif** (Tools → Board → Boards Manager → "esp32").
  Both 2.0.x and 3.x are supported; the code guards the APIs that differ between them.
- Libraries (Tools → Manage Libraries):
  - `DHT sensor library` by Adafruit
  - `Adafruit Unified Sensor`

  Nothing else. Networking, BLE, NVS and the web server all use built-in ESP32 APIs, and
  the dashboard JSON is hand-rolled — so there is no ArduinoJson / AsyncWebServer
  version-mismatch risk on demo day.

### 4.2 Board settings (all four boards)

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| **Partition Scheme** | **Huge APP (3MB No OTA / 1MB SPIFFS)** |
| Flash Size | 4MB (32Mb) |
| Upload Speed | 921600 |
| Core Debug Level | None |

> The **Huge APP** partition is mandatory. BLE + ESP-NOW + the web server will not fit in
> the default 1.2 MB application partition.

### 4.3 Wiring

See **[docs/WIRING.md](docs/WIRING.md)** for the full circuit setup, including the
mandatory 3.3 V supply rule for both MQ sensors and the ADC1 pin requirement.

### 4.4 Collect the MAC addresses

1. Open `tools/print_mac/print_mac.ino`, upload it to each board in turn.
2. Note the printed `STA MAC` for each.
3. Paste all four into the `NODE_MAC` table in **`common/wsn_config.cpp`**.

### 4.5 Set the anchor coordinates

In `common/wsn_config.cpp`, set `NODE_POS[]` to the tape-measured positions (metres) of
N2, N3 and N4 in the lab, and N1's true position for the localization-error calculation.
Defaults assume a 4.0 m × 3.0 m area.

### 4.6 Build and flash

```bash
python tools/sync_common.py          # copy common/ into the four sketch folders
```

Then in Arduino IDE, open and upload each sketch to its board:

| Sketch | Board |
|---|---|
| `firmware/Node1_Sensor/Node1_Sensor.ino` | the board with the DHT22 |
| `firmware/Node2_SensorRelay/Node2_SensorRelay.ino` | the board with the MQ2 |
| `firmware/Node3_ClusterHead/Node3_ClusterHead.ino` | the board with the MQ135 |
| `firmware/Node4_Sink/Node4_Sink.ino` | the board with the PIR |

### 4.7 Open the dashboard

1. Power all four nodes.
2. On a phone or laptop, join the Wi-Fi network **`WSN_SINK`** (password `wsnlab2024`).
3. Browse to **<http://192.168.4.1>**.

---

## 5. Running the demonstration

Follow **[docs/DEMO_SCRIPT.md](docs/DEMO_SCRIPT.md)**. The condensed sequence:

| Step | Action | What to show |
|---|---|---|
| 1 | Power on | Boot banner, role, MAC, warm-up countdown |
| 2 | Wait for Phase A | BLE advertising/scanning, neighbor discovery |
| 3 | Read N1 serial | RSSI per anchor, estimated distances, estimated (x,y), localization error |
| 4 | Phase B starts | Beacon with slot map, time sync, nodes adopting schedule |
| 5 | Watch any node | `CCA busy → backoff=N → TX → ACK` lines |
| 6 | `collide` on N4 | Two nodes contend in one slot; backoff resolves it |
| 7 | `collide hard` | Forced collision → ACK timeout → CW doubles → successful retransmission |
| 8 | Breathe on the MQ2 | Alarm → N3 grants the dynamic slot → `schedVersion` increments with a reason |
| 9 | Watch N3 | 3 raw packets → 1 aggregated packet, data reduction % |
| 10 | Watch N4 | Multi-hop delivery `1→3→4`, PDR, delay, throughput |
| 11 | `fail 3` (or unplug N3) | Link failure → detection → route update → `1→2→4` → delivery continues |
| 12 | `recover 3` (or replug) | Primary route restored automatically; failover latency reported |
| 13 | Dashboard | All 7 panels live |
| 14 | `python tools/plot_metrics.py` | Performance graphs for the report |

### Serial commands (available on every node)

| Command | Effect |
|---|---|
| `help` | List all commands |
| `stats` | MAC, routing and metric counters |
| `nbr` | Neighbor table with RSSI, ACK rate and cost |
| `route` | Current next hop, hop count and full path |
| `sched` | Live slot map, schedule version and last change reason |
| `sensors` | Raw and converted sensor values, health flags |
| `loc` | Last BLE localization result |
| `calib` | Calibrate MQ R0 in clean air (saved to NVS) |
| `calib1m <id>` | Record BLE RSSI at exactly 1 m from node `<id>` |
| `settrue <x> <y>` | Set N1's tape-measured true position |
| `fail <id>` / `recover <id>` | Simulate / clear a link failure |
| `collide` / `collide hard` | Contention and forced-collision demonstrations |
| `sleepinfo` | Duty cycle and sleep accounting |
| `csv on` / `csv off` | Toggle the machine-readable CSV stream |

---

## 6. Performance metrics

All seven metrics named in the guidelines are measured (the requirement is four):

| Metric | Where measured | How |
|---|---|---|
| Packet Delivery Ratio | N4 | `received / expected` per source, from sequence numbers |
| Packet loss | N4 | `expected − received`, plus loss rate per link |
| End-to-end delay | N4 | `sinkNetTime − frame.txTimeMs`, using beacon time sync |
| Throughput | N4 | delivered payload bits / elapsed seconds |
| Retransmissions | every node | MAC retry counter, reported to the sink |
| Localization error | N1 | `‖estimated − true‖₂`, with running mean and RMSE |
| Data reduction % | N3 | `(1 − aggBytes/rawBytes) × 100`, byte-wise and packet-wise |

---

## 7. Documentation

| Document | Contents |
|---|---|
| [docs/WIRING.md](docs/WIRING.md) | Circuit/network setup, pin tables, power budget, safety |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Architecture diagram, protocol stack, frame formats |
| [docs/FLOWCHART.md](docs/FLOWCHART.md) | State machines and protocol flowcharts |
| [docs/DEMO_SCRIPT.md](docs/DEMO_SCRIPT.md) | Step-by-step demonstration script |
| [docs/VIVA_QA.md](docs/VIVA_QA.md) | Viva preparation |
| [docs/RESULTS.md](docs/RESULTS.md) | Measured values, tables and graphs |
| [docs/TECHNICAL_REPORT.md](docs/TECHNICAL_REPORT.md) | Short technical report |

---

## 8. License

Academic coursework. Reuse freely with attribution.
