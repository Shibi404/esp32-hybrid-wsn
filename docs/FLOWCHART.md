# Flowcharts

**23CSE341 WSN — Lab Evaluation 2** · deliverable: *Flowchart*

---

## 1. Node lifecycle (all four nodes)

```
                        ┌───────────┐
                        │   POWER   │
                        └─────┬─────┘
                              ▼
              ┌───────────────────────────────┐
              │ Wi-Fi up (STA, channel 1)     │
              │ sink also raises SoftAP       │
              │ sensors init, warm-up starts  │
              │ metrics / route / TDMA init   │
              │ ESP-NOW + CSMA/CA up          │
              └───────────────┬───────────────┘
                              ▼
                      ┌───────────────┐        sink only:
                      │  role == SINK?│───yes──▶ skip to PHASE B
                      └───────┬───────┘          and stay there
                              │ no
    ┌─────────────────────────▼─────────────────────────┐
    │                   PHASE A · BLE                   │
    │                                                   │
    │   advertise "WSN-Nx"                              │
    │   active scan 4 s, duplicates enabled             │
    │            │                                      │
    │            ▼                                      │
    │   ≥3 samples per neighbour?──no──▶ ignore it      │
    │            │ yes                                  │
    │            ▼                                      │
    │   median → EWMA → log-distance → distance         │
    │            │                                      │
    │            ├──▶ argmax RSSI = nearest node        │
    │            │                                      │
    │            ▼                                      │
    │   ≥3 anchors heard?──no──▶ proximity result only  │
    │            │ yes                                  │
    │            ▼                                      │
    │   trilaterate → (x̂, ŷ)                            │
    │   error = ‖(x̂,ŷ) − truth‖ ; update mean, RMSE     │
    │            │                                      │
    │            ▼                                      │
    │   feed BLE RSSI into the routing metric           │
    │   BLEDevice::deinit(true)  → ~60 kB freed         │
    └─────────────────────────┬─────────────────────────┘
                              ▼
    ┌─────────────────────────────────────────────────────┐
    │                   PHASE B · Wi-Fi                   │
    │   (N1 first sends its FT_LOC result to the sink)    │
    │                                                     │
    │   ┌──────────── every loop() pass ──────────────┐   │
    │   │  CLI · sensors · MAC · TDMA · routing       │   │
    │   │  slot actions (next flowchart)              │   │
    │   │  sleep if this slot is neither owned        │   │
    │   │  nor needs listening                        │   │
    │   └─────────────────────────────────────────────┘   │
    │                                                     │
    │   30 s elapsed?──no──▶ keep going                   │
    │            │ yes                                    │
    └────────────┼────────────────────────────────────────┘
                 └──────────▶ back to PHASE A
                              (refreshes localization live)
```

---

## 2. Slot actions, once per superframe

```
          ┌──────────────────────────────┐
          │ playing dead (`fail <self>`)?│──yes──▶ transmit nothing
          └──────────────┬───────────────┘
                         │ no
          ┌──────────────▼───────────────┐
          │ time-synced to a beacon?     │──no───▶ listen only
          └──────────────┬───────────────┘          (cluster head exempt)
                         │ yes
          ┌──────────────▼───────────────┐
          │ past the leading guard band? │──no───▶ wait
          └──────────────┬───────────────┘
                         │ yes
     ┌───────────────────┼────────────────────────────────────┐
     ▼                   ▼                   ▼                ▼
 slot 0 BEACON      my DATA slot        slot 4 AGG       slot 5 CAP
 (cluster head)     (N1 / N2)           (cluster head)   (everyone)
     │                   │                   │                │
     ▼                   ▼                   ▼                ▼
 review schedule    read sensors        add own reading   send HELLO
 (grant/revoke)     pick next hop       build aggregate   announce any
     │              build DATA          min/max/mean      pending route
     ▼                   │              OR alarms         event
 build + broadcast       ▼                   │                │
 BEACON             enqueue to MAC      nothing held?     collision test
 (slot map,              │              ──yes──▶ slot      armed? also
  version, reason,       ▼                      unused     send DATA now
  network time)     mark slot used            │
                                               ▼
                                          enqueue AGG
                                          to next hop
     ┌───────────────────────────────────────┐
     ▼                                       ▼
 slot 6 STATUS (sink)              every slot: owned?
     │                                       │
     ▼                                 no, and nothing
 broadcast NET_STATUS                  to listen for
 emit a CSV row                              │
                                             ▼
                                   esp_wifi_stop() for the
                                   rest of the slot; LED off
```

---

## 3. CSMA/CA MAC

```
                    ┌──────────────────┐
                    │ frame enqueued   │
                    └────────┬─────────┘
                             ▼
   ┌─────────────────────────────────────────────────┐
   │ [1] CARRIER SENSE                               │
   │                                                 │
   │   physical CCA: any 802.11 frame above          │
   │                 −75 dBm within the last 3 ms?   │
   │              OR                                 │
   │   virtual  NAV: is another node's declared      │
   │                 durationUs still running?       │
   └────────────────────┬────────────────────────────┘
                        │
              ┌─────────▼─────────┐
              │   medium busy?    │
              └───┬───────────┬───┘
                 yes          no
                  │           │
                  ▼           ▼
       ┌────────────────┐  ┌─────────────────────────────┐
       │ busy > 50 ms?  │  │ [2] idle for a full DIFS    │
       │   yes → send   │  │     (2 ms continuously)?    │
       │   anyway,      │  └──────────┬──────────────────┘
       │   count as     │             │ yes
       │   "forced"     │             ▼
       │   no → defer   │  ┌─────────────────────────────┐
       └────────────────┘  │ [3] backoff = rand(0, CW)   │
                           │     slots of 500 µs         │
                           └──────────┬──────────────────┘
                                      ▼
                    ┌──────────────────────────────────┐
                    │  medium busy mid-countdown?      │
                    │     YES → FREEZE the counter     │◀── this is how
                    │           resume where it        │    contention is
                    │           stopped, do not redraw │    resolved
                    │     NO  → decrement each slot    │
                    └──────────────┬───────────────────┘
                                   │ counter == 0
                                   ▼
                        ┌─────────────────────┐
                        │ [4] TRANSMIT        │
                        │ stamp txTimeMs on   │
                        │ the FIRST try only  │
                        │ set local NAV       │
                        └──────────┬──────────┘
                                   ▼
                        ┌─────────────────────┐
                        │ broadcast?          │──yes──▶ done, no ACK
                        └──────────┬──────────┘         (802.11 semantics)
                                   │ no
                                   ▼
                     ┌───────────────────────────┐
                     │ [5] wait 60 ms for an ACK │
                     └────┬─────────────────┬────┘
                      ACK │                 │ timeout
                          ▼                 ▼
          ┌───────────────────────┐  ┌──────────────────────────┐
          │ CW := CW_MIN (8)      │  │ retries++                │
          │ record RTT            │  │ CW := min(2·CW, 64)      │
          │ credit the link       │  │ debit the link           │
          │ pop the frame         │  └────────┬─────────────────┘
          └───────────────────────┘           ▼
                                    ┌──────────────────────┐
                                    │ retries > 4 ?        │
                                    └───┬──────────────┬───┘
                                       no             yes
                                        │              │
                                        ▼              ▼
                                   back to [1]   DROP the frame
                                                 mark link failing
                                                 → routing reacts
```

---

## 4. Dynamic schedule review (cluster head, once per superframe)

```
                 ┌────────────────────────────┐
                 │ any node reporting an alarm│
                 │ or a backlog ≥ 3?          │
                 └────┬──────────────────┬────┘
                     yes                 no
                      │                  │
                      ▼                  ▼
         ┌────────────────────┐  ┌───────────────────────┐
         │ already granted to │  │ dynamic slot currently│
         │ that node?         │  │ granted to anyone?    │
         └──┬──────────────┬──┘  └───┬───────────────┬───┘
           yes            no        yes              no
            │              │         │                │
            ▼              ▼         ▼                ▼
         nothing   grant slot 3   release slot 3   nothing
                   kind := DATA   kind := SLEEP
                   version++      version++
                   ALARM_GRANT    ALARM_RELEASE
                        │              │
                        └──────┬───────┘
                               ▼
         ┌─────────────────────────────────────────┐
         │ any DATA-slot owner silent for 3 slots? │
         │   yes → revoke, version++, NODE_SILENT  │
         └────────────────────┬────────────────────┘
                              ▼
         ┌─────────────────────────────────────────┐
         │ any revoked owner heard again?          │
         │   yes → restore, version++, NODE_REJOIN │
         └────────────────────┬────────────────────┘
                              ▼
                   age the silence counters
                              ▼
               broadcast the beacon with the
               new version and its reason
```

---

## 5. Route failure detection and recovery

```
   ┌──────────────────────┐        ┌──────────────────────────┐
   │ ACK timeout on a     │        │ no frame heard from a    │
   │ unicast to peer P    │        │ neighbour for 7 s        │
   │  → consecFail++      │        │                          │
   └──────────┬───────────┘        └────────────┬─────────────┘
              │ consecFail ≥ 3                  │
              └────────────────┬────────────────┘
                               ▼
              ┌────────────────────────────────────┐
              │ mark link to P DOWN                │
              │ cost(P) := ∞                       │
              │ record the detection timestamp     │
              └────────────────┬───────────────────┘
                               ▼
              ┌────────────────────────────────────┐
              │ recompute cost for every neighbour │
              │ pick the minimum with a known path │
              └────────────────┬───────────────────┘
                               ▼
                    ┌──────────────────────┐
                    │ a candidate exists?  │
                    └───┬──────────────┬───┘
                       yes            no
                        │              │
                        ▼              ▼
     ┌──────────────────────────┐  ┌────────────────────────┐
     │ next hop := candidate    │  │ node isolated; hold     │
     │ failover = now − detect  │  │ readings until a        │
     │ broadcast ROUTE_UPDATE   │  │ neighbour returns       │
     │ traffic continues; the   │  └────────────────────────┘
     │ sink sees a new path     │
     └──────────┬───────────────┘
                ▼
     ┌──────────────────────────────────────────────┐
     │ RECOVERY: P is heard again                   │
     │   link marked up, consecFail := 0            │
     │   costs recomputed                           │
     │   the original path wins back ON COST,       │
     │   not by being forced                        │
     └──────────────────────────────────────────────┘
```

---

## 6. Sensor read with fault handling

```
            ┌──────────────────────┐
            │ sample tick (1 s)    │
            └──────────┬───────────┘
                       ▼
        ┌──────────────────────────────┐
        │ which sensor does this node  │
        │ carry? (runtime, from NODE_ID)│
        └───┬───────────┬──────────┬───┘
         DHT22        MQ2/MQ135    PIR
            │           │           │
            ▼           ▼           ▼
   ┌────────────┐ ┌──────────────┐ ┌──────────────┐
   │ ≥2.2 s     │ │ 16× ADC      │ │ read level + │
   │ since last?│ │ multisample  │ │ ISR edge     │
   │ read T, RH │ │              │ │ count        │
   └─────┬──────┘ └──────┬───────┘ └──────┬───────┘
         ▼               ▼                ▼
   ┌───────────┐  ┌──────────────┐   set SENS_MOTION
   │ NaN or    │  │ raw < 8 or   │   set/clear ALARM_MOTION
   │ out of    │  │ raw > 4080?  │
   │ range?    │  │ (open or     │
   └──┬─────┬──┘  │  saturated)  │
     yes    no    └───┬──────┬───┘
      │      │       yes     no
      │      │        │      │
      ▼      ▼        ▼      ▼
  fails++  publish  fails++  Rs = ((Vcc/Vout)−1)·RL
      │    clear      │            │
      ▼    fails      ▼            ▼
 ┌─────────────┐  ┌─────────┐  ┌──────────────────────┐
 │ fails ≥ 3?  │  │ same    │  │ R0 known?            │
 │  → clear    │  │ rule    │  │  no  → auto-calibrate│
 │    SENS_*   │  └─────────┘  │        once warm     │
 │    bit, set │               │  yes → ppm = a·ratio^b│
 │    NAN      │               │        clamp to the  │
 │  → the      │               │        datasheet span│
 │    aggregator│              └──────────┬───────────┘
 │    now skips │                         ▼
 │    the field │              set/clear ALARM_GAS
 │    instead of│
 │    averaging │     ◀── a failed read is never published as 0
 │    a fake 0  │
 └─────────────┘
```
