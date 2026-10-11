/* =============================================================================
 *  wsn_node.cpp  --  The shared node runtime: phase machine, slot actions and
 *                    frame handling for all four roles.
 * ========================================================================== */

#include "wsn_node.h"
#include "wsn_log.h"
#include "wsn_ble.h"
#include "wsn_mac.h"
#include "wsn_tdma.h"
#include "wsn_route.h"
#include "wsn_sensors.h"
#include "wsn_metrics.h"
#include "wsn_aggregate.h"
#include "wsn_cli.h"
#include <WiFi.h>
#include <esp_wifi.h>

#define TAG "NODE"

uint8_t  NODE_ID   = 0;
NodeRole NODE_ROLE = ROLE_SENSOR;

static NodeHooks s_hooks;
static NodePhase s_phase       = PHASE_BOOT;
static uint32_t  s_phaseStart  = 0;
static uint32_t  s_bootMs      = 0;
static uint32_t  s_reportsSent = 0;
static bool      s_simDown     = false;

static NodeView   s_view[NUM_NODES + 1];
static LocPayload s_lastLoc;   static bool s_hasLoc = false;
static AggPayload s_lastAgg;   static bool s_hasAgg = false;

/* Per-superframe edge tracking, so a slot action fires once and not every
 * pass through loop().                                                       */
static uint32_t s_lastBeaconSf   = 0xFFFFFFFF;
static uint32_t s_lastDataSf     = 0xFFFFFFFF;
static uint32_t s_lastAggSf      = 0xFFFFFFFF;
static uint32_t s_lastHelloSf    = 0xFFFFFFFF;
static uint32_t s_lastStatusSf   = 0xFFFFFFFF;
static uint32_t s_lastReviewSf   = 0xFFFFFFFF;
static bool     s_collideArmed   = false;
static bool     s_collideHard    = false;

const char *wsnNodePhaseName(NodePhase p) {
  switch (p) {
    case PHASE_BOOT:      return "BOOT";
    case PHASE_BLE:       return "BLE DISCOVERY / LOCALIZATION";
    case PHASE_WIFI_INIT: return "WI-FI INITIALISATION";
    case PHASE_WIFI:      return "WI-FI NETWORK OPERATION";
    default:              return "?";
  }
}

NodePhase wsnNodePhase(void)   { return s_phase; }
uint32_t  wsnNodeUptimeS(void) { return (millis() - s_bootMs) / 1000; }
uint32_t  wsnNodeReportsSent(void) { return s_reportsSent; }
bool      wsnNodeIsSimulatedDown(void) { return s_simDown; }

const NodeView   *wsnNodeView(uint8_t id) {
  return (id <= NUM_NODES) ? &s_view[id] : &s_view[0];
}
const LocPayload *wsnNodeLastLocalization(void) { return s_hasLoc ? &s_lastLoc : nullptr; }
const AggPayload *wsnNodeLastAggregate(void)    { return s_hasAgg ? &s_lastAgg : nullptr; }

void wsnNodeSetSimulatedDown(bool down) {
  s_simDown = down;
  if (down) {
    logSection("SIMULATED NODE FAILURE - THIS NODE IS NOW 'DEAD'");
    LOGE(TAG, "All transmissions suppressed and all frames ignored (except "
              "control commands). Neighbours will detect the failure and "
              "re-route around this node.");
    digitalWrite(PIN_LED, LOW);
  } else {
    logSection("NODE RECOVERED - RESUMING NORMAL OPERATION");
    LOGI(TAG, "Transmissions resumed; neighbours should restore their routes.");
  }
}

/* ========================================================================== */
/*  Wi-Fi                                                                     */
/* ========================================================================== */

static void wifiBringUp(void) {
  /* STA mode for ESP-NOW.  The sink additionally raises its SoftAP, on the
   * SAME channel -- ESP-NOW and the AP share one radio, so a mismatched
   * channel silently breaks one of them.  That is the classic failure here.  */
  if (NODE_ROLE == ROLE_SINK) WiFi.mode(WIFI_AP_STA);
  else                        WiFi.mode(WIFI_STA);

  WiFi.disconnect(false, false);

  esp_wifi_set_ps(WIFI_PS_NONE);     /* power save off: it delays ESP-NOW RX  */
  esp_wifi_set_channel(WSN_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  char mac[18];
  uint8_t m[6];
  WiFi.macAddress(m);
  LOGI(TAG, "Wi-Fi up: STA MAC %s, channel %d", logMacStr(m, mac), WSN_WIFI_CHANNEL);
}

/* ========================================================================== */
/*  Frame handling                                                            */
/* ========================================================================== */

static void noteView(const WsnHeader *h, int8_t rssi) {
  if (h->src < 1 || h->src > NUM_NODES) return;
  NodeView &v = s_view[h->src];
  v.seen        = true;
  v.lastHeardMs = millis();
  if (rssi) v.rssi = rssi;
}

static void sendDataFrame(void);

static void onFrame(const WsnHeader *h, const uint8_t *pl, size_t plen, int8_t rssi) {

  /* A node told to play dead answers nothing but control commands -- that is
   * what makes the failure look real to its neighbours while still letting an
   * operator bring it back.                                                  */
  if (s_simDown && h->type != FT_CMD) return;

  noteView(h, rssi);

  switch (h->type) {

    /* ---- BROADCAST: schedule + time sync --------------------------------- */
    case FT_BEACON: {
      const BeaconPayload *b = (const BeaconPayload *)pl;
      tdmaApplyBeacon(b, millis());
      s_view[h->src].schedVersion = b->schedVersion;
      /* A beacon carries no hop count, so keep whatever the head's own HELLO
       * advertised and fall back to 1 only to bootstrap -- the head is one hop
       * from the sink in the designed topology.                             */
      {
        const uint8_t hops = (s_view[h->src].hopsToSink != 0xFF)
                           ? s_view[h->src].hopsToSink : 1;
        routeNoteRx(h->src, rssi, hops, ROLE_CLUSTER_HEAD);
      }
      break;
    }

    /* ---- BROADCAST: neighbour discovery / liveness ------------------------ */
    case FT_HELLO: {
      const HelloPayload *p = (const HelloPayload *)pl;
      routeNoteRx(h->src, rssi, p->hopsToSink, p->role);
      NodeView &v = s_view[h->src];
      v.role         = p->role;
      v.hopsToSink   = p->hopsToSink;
      v.nextHop      = p->nextHop;
      v.retx         = p->retxTotal;
      v.alarm        = p->alarm;
      v.schedVersion = p->schedVersion;

      if (NODE_ID == CLUSTER_HEAD_ID) tdmaNoteNodeReport(h->src, p->alarm, 0);
      break;
    }

    /* ---- UNICAST: raw sensor data ---------------------------------------- */
    case FT_DATA: {
      const DataPayload *d = (const DataPayload *)pl;
      NodeView &v = s_view[h->src];
      v.hasData  = true;
      v.last     = *d;
      v.retx     = d->retxTotal;
      v.alarm    = d->alarm;
      v.lastHops = h->hop;
      wsnPathToString(h, v.lastPath, sizeof(v.lastPath));

      if (NODE_ID == CLUSTER_HEAD_ID) {
        tdmaNoteNodeActive(h->src);
        tdmaNoteNodeReport(h->src, d->alarm, d->queueLen);
        /* AGGREGATION: hold the reading instead of forwarding it.  This is
         * exactly where the data reduction comes from -- three packets in,
         * one out, once per superframe.                                     */
        aggAddSample(h->src, d, h->txTimeMs, WSN_HDR_LEN + plen + 2);

      } else if (NODE_ID == SINK_ID) {
        /* Direct delivery (the one-hop fallback route).                     */
        metricsNoteRx(h, WSN_HDR_LEN + plen + 2, plen, d->retxTotal,
                      tdmaNetworkTimeMs());

      } else {
        /* RELAY: this node is on someone else's alternate route.           */
        const uint8_t next = routeNextHop(h->finalDst);
        if (next && next != h->src) {
          LOGI(TAG, "RELAYING %s seq=%u from N%u -> next hop N%u (final N%u)",
               wsnFrameTypeName(h->type), h->seq, h->src, next, h->finalDst);
          macForward(h, pl, plen, next);
        } else {
          LOGW(TAG, "Cannot relay seq=%u from N%u - no usable next hop",
               h->seq, h->src);
        }
      }
      break;
    }

    /* ---- UNICAST: aggregated data ---------------------------------------- */
    case FT_AGG: {
      const AggPayload *a = (const AggPayload *)pl;
      if (h->finalDst == NODE_ID || NODE_ID == SINK_ID) {
        s_lastAgg = *a;
        s_hasAgg  = true;
        metricsNoteRx(h, WSN_HDR_LEN + plen + 2, plen, a->retxTotal,
                      tdmaNetworkTimeMs());
        /* Credit the aggregate's byte saving at the sink too, so the sink's
         * dashboard can show the reduction without having to ask N3.        */
        metricsNoteAggregation(a->rawPackets, a->rawBytes, a->aggBytes);
        char path[20];
        wsnPathToString(h, path, sizeof(path));
        LOGI(TAG, "AGGREGATE from N%u: %u readings, sources mask 0x%02X, "
                  "alarms 0x%02X, temp avg %.1f, gas avg %.0f (path %s)",
             h->src, a->nSamples, a->srcMask, a->alarmOr, a->tAvg, a->gAvg, path);
      } else {
        const uint8_t next = routeNextHop(h->finalDst);
        if (next) macForward(h, pl, plen, next);
      }
      break;
    }

    /* ---- UNICAST: localization result ------------------------------------ */
    case FT_LOC: {
      const LocPayload *p = (const LocPayload *)pl;
      if (h->finalDst == NODE_ID || NODE_ID == SINK_ID) {
        s_lastLoc = *p;
        s_hasLoc  = true;
        LOGI(TAG, "LOCALIZATION from N%u: est (%.2f, %.2f) true (%.2f, %.2f) "
                  "error %.2f m, nearest N%u, %u anchors",
             p->nodeId, p->estX, p->estY, p->trueX, p->trueY, p->errorM,
             p->nearestId, p->nAnchors);
      } else {
        const uint8_t next = routeNextHop(h->finalDst);
        if (next) macForward(h, pl, plen, next);
      }
      break;
    }

    /* ---- BROADCAST: route control ---------------------------------------- */
    case FT_ROUTE_UPDATE: {
      const RoutePayload *p = (const RoutePayload *)pl;
      LOGW(TAG, "ROUTE CONTROL from N%u: %s on link N%u-N%u; its next hop is "
                "now N%u (%u hops, cost %u)",
           p->originator, wsnRouteEventName(p->event), p->peerA, p->peerB,
           p->newNextHop, p->hopsToSink, p->cost);

      s_view[p->originator].nextHop    = p->newNextHop;
      s_view[p->originator].hopsToSink = p->hopsToSink;

      /* Another node reporting a dead peer is evidence, not proof -- it may
       * simply be out of range of it.  Recompute rather than blindly marking
       * the link down here.                                                 */
      routeRecompute();
      break;
    }

    /* ---- BROADCAST: network status from the sink -------------------------- */
    case FT_NET_STATUS: {
      const NetStatusPayload *p = (const NetStatusPayload *)pl;
      LOGD(TAG, "NET STATUS: PDR %.2f%%, rx %lu, lost %lu, delay %u ms, "
                "%u bit/s, reduction %.2f%%",
           p->pdrX100 / 100.0f, (unsigned long)p->rxTotal,
           (unsigned long)p->lost, p->avgDelayMs, p->throughputBps,
           p->reductionX100 / 100.0f);
      routeNoteRx(h->src, rssi, 0, ROLE_SINK);
      break;
    }

    /* ---- BROADCAST: demonstration commands -------------------------------- */
    case FT_CMD: {
      const CmdPayload *c = (const CmdPayload *)pl;
      if (c->target != BROADCAST_ID && c->target != NODE_ID) break;

      switch (c->cmd) {
        /* Only the named node acts on this.  Its neighbours are deliberately
         * NOT told the link is gone -- they have to DETECT it themselves
         * through ACK timeouts and silence, which is precisely the step the
         * evaluation asks us to demonstrate.  Telling them would skip it.
         * (`faillink <id>` on a console forces one local link down instead,
         * for when you want to steer a specific route by hand.)            */
        case CMD_FAIL_LINK:
          if ((uint8_t)c->arg == NODE_ID) wsnNodeSetSimulatedDown(true);
          break;
        case CMD_RECOVER_LINK:
          if ((uint8_t)c->arg == NODE_ID) wsnNodeSetSimulatedDown(false);
          break;
        case CMD_COLLIDE:
          if (NODE_ID == NODE_N1 || NODE_ID == NODE_N2) {
            s_collideArmed = true;
            s_collideHard  = (c->arg != 0);
            LOGW(TAG, "COLLISION TEST ARMED on this node (%s)",
                 s_collideHard ? "carrier sense BYPASSED" : "carrier sense active");
          }
          if (NODE_ID == CLUSTER_HEAD_ID) tdmaScheduleCollision(NODE_N1, NODE_N2);
          break;
        case CMD_CALIBRATE:   sensorsCalibrate(); break;
        case CMD_RESET_STATS: macResetStats(); metricsReset(); break;
        default: break;
      }
      break;
    }

    /* ---- UNICAST: slot request to the cluster head ------------------------ */
    case FT_SLOT_REQ: {
      if (NODE_ID != CLUSTER_HEAD_ID) break;
      const SlotReqPayload *p = (const SlotReqPayload *)pl;
      LOGI(TAG, "SLOT REQUEST from N%u: queue=%u alarm=0x%02X wants %u slot(s)",
           h->src, p->queueLen, p->alarm, p->wantSlots);
      tdmaNoteNodeReport(h->src, p->alarm, p->queueLen);
      break;
    }

    default: break;
  }

  if (s_hooks.onFrame) s_hooks.onFrame(h, pl, plen, rssi);
}

/* ========================================================================== */
/*  Transmit actions                                                          */
/* ========================================================================== */

static void fillHello(HelloPayload *p) {
  memset(p, 0, sizeof(*p));
  p->role         = (uint8_t)NODE_ROLE;
  p->hopsToSink   = routeHopsToSink();
  p->cost         = routeOwnCost();
  p->nextHop      = routeNextHop(SINK_ID);
  p->schedVersion = tdmaSchedule()->version;
  p->retxTotal    = (uint16_t)macStats()->retransmissions;
  p->alarm        = sensorsGet()->alarm;

  uint8_t alive = 0;
  const Neighbor *n = routeNeighbors();
  for (uint8_t i = 1; i <= NUM_NODES; i++)
    if (n[i].known && n[i].linkUp) alive |= (uint8_t)(1u << (i - 1));
  p->aliveMask = alive;
}

static void sendDataFrame(void) {
  DataPayload d;
  sensorsFillPayload(&d);
  d.queueLen    = macQueueLen();
  d.retxTotal   = (uint16_t)macStats()->retransmissions;
  d.nextHopRssi = macPeerRssi(routeNextHop(SINK_ID));
  if (routeIsUsingAlternate()) d.flags |= DFLAG_RELAYED;

  const uint8_t next = routeNextHop(SINK_ID);
  if (!next) {
    LOGW(TAG, "No route to the sink - holding this reading");
    return;
  }

  if (macSend(FT_DATA, next, SINK_ID, &d, sizeof(d))) {
    s_reportsSent++;
    tdmaMarkSlotUsed();
    LOGI(TAG, "Sensor report #%lu queued: next hop N%u, final N%u%s",
         (unsigned long)s_reportsSent, next, SINK_ID,
         routeIsUsingAlternate() ? "  [via ALTERNATE route]" : "");
  }
}

static void sendAggregate(void) {
  /* The cluster head folds in its own reading last, so its sensor is part of
   * the aggregate rather than a separate packet.                             */
  DataPayload own;
  sensorsFillPayload(&own);
  own.retxTotal = (uint16_t)macStats()->retransmissions;
  aggAddLocal(&own, tdmaNetworkTimeMs());

  AggPayload a;
  if (!aggBuild(&a)) {
    LOGI(TAG, "Nothing to aggregate this superframe - uplink slot unused");
    return;
  }
  a.retxTotal = (uint8_t)min((uint32_t)255, macStats()->retransmissions);

  const uint8_t next = routeNextHop(SINK_ID);
  if (!next) { LOGW(TAG, "No route to the sink - aggregate held"); return; }

  if (macSend(FT_AGG, next, SINK_ID, &a, sizeof(a))) {
    tdmaMarkSlotUsed();
    LOGI(TAG, "Aggregate uplinked: next hop N%u, final N%u", next, SINK_ID);
  }
}

static void sendNetStatus(void) {
  NetStatusPayload p;
  memset(&p, 0, sizeof(p));
  p.rxTotal       = metricsRxOverall();
  p.lost          = metricsLostOverall();
  p.expected      = p.rxTotal + p.lost;
  p.retxTotal     = metricsRetxOverall();
  p.pdrX100       = (uint16_t)(metricsPdrOverall() * 10000.0f / 100.0f);
  p.avgDelayMs    = (uint16_t)min((uint32_t)65535, metricsAvgDelayMs());
  p.throughputBps = (uint16_t)min(65535.0f, metricsThroughputBps());
  p.reductionX100 = (uint16_t)(metricsDataReductionPct() * 100.0f);

  uint8_t alive = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++)
    if (s_view[i].seen && (millis() - s_view[i].lastHeardMs) < NEIGHBOR_TIMEOUT_MS)
      alive |= (uint8_t)(1u << (i - 1));
  p.aliveMask = alive;

  macSend(FT_NET_STATUS, BROADCAST_ID, BROADCAST_ID, &p, sizeof(p));
  tdmaMarkSlotUsed();
}

static void announceRouteIfPending(void) {
  uint8_t ev, a, b;
  if (!routeTakePendingAnnounce(&ev, &a, &b)) return;

  RoutePayload p;
  routeFillRoutePayload(&p, ev, a, b);
  macSend(FT_ROUTE_UPDATE, BROADCAST_ID, BROADCAST_ID, &p, sizeof(p));
  LOGI(TAG, "Broadcasting route control: %s for link N%u-N%u",
       wsnRouteEventName(ev), a, b);
}

/* ========================================================================== */
/*  Slot actions                                                              */
/* ========================================================================== */

static void runSlotActions(void) {
  if (s_simDown) return;
  if (!tdmaIsSynced() && NODE_ID != CLUSTER_HEAD_ID) return;

  const uint8_t  slot = tdmaCurrentSlot();
  const uint32_t sf   = tdmaSuperframeIndex();
  const uint32_t el   = tdmaSlotElapsedMs();

  /* Everything below fires once per superframe, after the leading guard band
   * so transmissions never straddle a slot boundary.                         */
  if (el < TDMA_GUARD_MS) return;

  /* --- cluster head: review the schedule, then beacon it ------------------- */
  if (NODE_ID == CLUSTER_HEAD_ID && slot == SLOT_BEACON && s_lastBeaconSf != sf) {
    s_lastBeaconSf = sf;
    if (s_lastReviewSf != sf) { s_lastReviewSf = sf; tdmaReviewSchedule(); }

    BeaconPayload b;
    tdmaBuildBeacon(&b);
    macSend(FT_BEACON, BROADCAST_ID, BROADCAST_ID, &b, sizeof(b));
    tdmaMarkSlotUsed();
  }

  /* --- sensor nodes: transmit in the slot they own ------------------------- */
  if (tdmaIsMySlot() && tdmaSlotKind(slot) == SK_DATA && s_lastDataSf != sf) {
    if (NODE_ROLE == ROLE_SENSOR || NODE_ROLE == ROLE_SENSOR_RELAY) {
      s_lastDataSf = sf;
      sendDataFrame();
    }
  }

  /* --- cluster head: uplink the aggregate ---------------------------------- */
  if (NODE_ID == CLUSTER_HEAD_ID && slot == SLOT_AGG_UPLINK && s_lastAggSf != sf) {
    s_lastAggSf = sf;
    sendAggregate();
  }

  /* --- sink: broadcast the network status ---------------------------------- */
  if (NODE_ID == SINK_ID && slot == SLOT_SINK_STATUS && s_lastStatusSf != sf) {
    s_lastStatusSf = sf;
    sendNetStatus();
    metricsEmitCsv();
  }

  /* --- everyone: HELLO + route control in the contention slot --------------- */
  if (tdmaSlotKind(slot) == SK_CAP && s_lastHelloSf != sf) {
    s_lastHelloSf = sf;

    /* The contention period is where CSMA/CA earns its keep: all four nodes
     * want it at once, every superframe.  That is also what makes the
     * backoff-freeze counter climb during a normal run.                     */
    HelloPayload hp;
    fillHello(&hp);
    macSend(FT_HELLO, BROADCAST_ID, BROADCAST_ID, &hp, sizeof(hp));

    announceRouteIfPending();

    /* Collision demonstration: N1 and N2 were both granted this slot, so they
     * both have a unicast ready at the same instant.                        */
    if (s_collideArmed) {
      s_collideArmed = false;
      if (s_collideHard) macForceCollision(1);
      LOGW(TAG, "COLLISION TEST: transmitting now, simultaneously with the "
                "other test node");
      sendDataFrame();
    }
  }
}

/* ========================================================================== */
/*  Phase machine                                                             */
/* ========================================================================== */

static void enterPhase(NodePhase p) {
  s_phase      = p;
  s_phaseStart = millis();
  logSection(wsnNodePhaseName(p));
}

static void runBlePhase(void) {
  /* One scan + solve per phase; the rest of the window is spent advertising
   * so the other nodes can range against this one.                           */
  static uint32_t lastRoundPhase = 0xFFFFFFFF;

  if (lastRoundPhase != s_phaseStart) {
    lastRoundPhase = s_phaseStart;

    bleRunRound();

    /* Feed the BLE RSSI into the routing metric.  This is the hand-off point
     * between the two halves of the hybrid network.                          */
    const BleNeighbor *nb = bleNeighbors();
    for (uint8_t i = 1; i <= NUM_NODES; i++)
      if (nb[i].seen) routeNoteBleRssi(i, nb[i].rssiMedian);

    /* N1 is the mobile node: it reports its position to the sink over Wi-Fi
     * in the next phase.                                                     */
    if (NODE_ID == NODE_N1) {
      bleFillLocPayload(&s_lastLoc);
      s_hasLoc = true;
    }
  }

  if (millis() - s_phaseStart >= BLE_PHASE_MS) enterPhase(PHASE_WIFI_INIT);
}

static void runWifiInit(void) {
  /* Release the BLE controller before the Wi-Fi phase.  The sink keeps
   * advertising so it stays usable as a localization anchor.                 */
  if (NODE_ROLE != ROLE_SINK || !SINK_BLE_ANCHOR) bleEnd();

  if (s_hooks.onWifiUp) s_hooks.onWifiUp();

  /* N1 reports the position it just computed, over Wi-Fi, to the sink.       */
  if (NODE_ID == NODE_N1 && s_hasLoc) {
    const uint8_t next = routeNextHop(SINK_ID);
    if (next) macSend(FT_LOC, next, SINK_ID, &s_lastLoc, sizeof(s_lastLoc));
  }

  enterPhase(PHASE_WIFI);
}

/* ========================================================================== */
/*  Setup / loop                                                              */
/* ========================================================================== */

void wsnNodeSetup(uint8_t id, NodeRole role, const NodeHooks *hooks) {
  NODE_ID   = id;
  NODE_ROLE = role;
  s_bootMs  = millis();
  memset(&s_hooks, 0, sizeof(s_hooks));
  if (hooks) s_hooks = *hooks;
  memset(s_view, 0, sizeof(s_view));
  for (uint8_t i = 0; i <= NUM_NODES; i++) s_view[i].hopsToSink = 0xFF;

  logBegin(SERIAL_BAUD);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  /* Wi-Fi comes up first and stays up for the whole run.  ESP-NOW peers and
   * (on the sink) the SoftAP therefore survive across BLE phases; only the
   * BLE stack is cycled.                                                     */
  wifiBringUp();
  logBanner();
  LOGI(TAG, "Sensor fitted: %s", sensorsName());

  sensorsBegin();
  metricsBegin();
  aggBegin();
  routeBegin();
  tdmaBegin();

  if (!macBegin(onFrame)) {
    LOGE(TAG, "MAC failed to start - halting");
    while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(150); }
  }

  cliBegin();

  /* The sink advertises permanently so it is always usable as a localization
   * anchor, but it never scans and never leaves the Wi-Fi phase: a blocking
   * 4 s BLE scan would stall the dashboard it has to keep serving.          */
  if (SINK_BLE_ANCHOR || NODE_ROLE != ROLE_SINK) bleBegin();

  if (NODE_ROLE == ROLE_SINK) enterPhase(PHASE_WIFI_INIT);
  else                        enterPhase(PHASE_BLE);
}

void wsnNodeLoop(void) {
  cliLoop();
  sensorsUpdate();

  switch (s_phase) {
    case PHASE_BLE:
      /* The MAC still runs so that control commands keep working, but no node
       * originates data while the network is ranging.                        */
      macLoop();
      runBlePhase();
      return;

    case PHASE_WIFI_INIT:
      runWifiInit();
      return;

    case PHASE_WIFI:
      break;

    default:
      enterPhase(PHASE_BLE);
      return;
  }

  macLoop();
  tdmaLoop();
  routeLoop();
  runSlotActions();

  if (s_hooks.onLoop) s_hooks.onLoop();

  /* Duty cycling: power the radio down for slots this node neither owns nor
   * needs to listen in.  Only worth it with enough of the slot left to pay
   * for the transition.                                                      */
  if (tdmaShouldSleepNow() && macQueueLen() == 0) {
    const uint32_t rem = tdmaSlotRemainingMs();
    if (rem > TDMA_GUARD_MS * 2) tdmaEnterSleep(rem - TDMA_GUARD_MS);
  }

  /* Return to BLE periodically so localization is refreshed live rather than
   * measured once at boot.  The sink is exempt -- it stays on the air.       */
  if (NODE_ROLE != ROLE_SINK && millis() - s_phaseStart >= WIFI_PHASE_MS) {
    if (s_hooks.onWifiDown) s_hooks.onWifiDown();
    bleBegin();
    enterPhase(PHASE_BLE);
  }
}

/* ========================================================================== */

void wsnNodeSendCommand(uint8_t target, uint8_t cmd, int32_t arg) {
  CmdPayload c;
  c.target = target;
  c.cmd    = cmd;
  c.arg    = arg;

  /* Apply it locally too -- a broadcast command addressed to everyone should
   * take effect here as well, and the node may be the target itself.         */
  if (target == BROADCAST_ID || target == NODE_ID) {
    WsnHeader h;
    wsnInitHeader(&h, FT_CMD, NODE_ID, BROADCAST_ID, BROADCAST_ID);
    onFrame(&h, (const uint8_t *)&c, sizeof(c), 0);
  }

  macSend(FT_CMD, BROADCAST_ID, BROADCAST_ID, &c, sizeof(c));
  LOGI(TAG, "Command broadcast: target N%u, cmd %u, arg %ld",
       target, cmd, (long)arg);
}
