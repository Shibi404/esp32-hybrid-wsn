/* =============================================================================
 *  wsn_route.cpp  --  Neighbour table, route metric, failure detection,
 *                     alternate-route selection and recovery.
 * ========================================================================== */

#include "wsn_route.h"
#include "wsn_log.h"
#include "wsn_mac.h"

#define TAG "ROUTE"

static Neighbor   s_nbr[NUM_NODES + 1];
static RouteState s_rt;
static bool       s_pendingAnnounce = false;
static uint8_t    s_announceEvent   = 0;
static uint8_t    s_announceA = 0, s_announceB = 0;

/* -------------------------------------------------------------------------- */

void routeBegin(void) {
  memset(s_nbr, 0, sizeof(s_nbr));
  memset(&s_rt, 0, sizeof(s_rt));

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    s_nbr[i].linkUp     = true;     /* optimistic until proven otherwise      */
    s_nbr[i].hopsToSink = 0xFF;     /* unknown                                */
    s_nbr[i].cost       = ROUTE_COST_INF;
  }

  if (NODE_ID == SINK_ID) {
    s_rt.hopsToSink = 0;            /* the sink is the destination            */
    s_rt.nextHop    = 0;
    s_rt.cost       = 0;
  } else {
    s_rt.hopsToSink = 0xFF;
    s_rt.nextHop    = 0;
    s_rt.cost       = ROUTE_COST_INF;
  }

  LOGI(TAG, "Routing up. Metric weights: hop=%d rssi=%d ack=%d loss=%d",
       ROUTE_W_HOP, ROUTE_W_RSSI, ROUTE_W_ACK, ROUTE_W_LOSS);
}

/* -------------------------------------------------------------------------- */

void routeNoteRx(uint8_t from, int8_t rssi, uint8_t hopsToSink, uint8_t role) {
  if (from < 1 || from > NUM_NODES || from == NODE_ID) return;

  Neighbor &n = s_nbr[from];
  const bool wasDown = !n.linkUp || !n.known;

  n.known       = true;
  n.lastHeardMs = millis();
  n.rxFrames++;
  n.hopsToSink  = hopsToSink;
  n.role        = role;
  if (rssi) n.wifiRssi = rssi;

  if (wasDown) {
    /* RECOVERY: hearing from a node that was marked down restores the link.
     * The route is not forced back -- it is recomputed, and the original path
     * wins only if its cost is genuinely better again.                       */
    n.linkUp     = true;
    n.consecFail = 0;
    s_rt.recoveries++;
    LOGI(TAG, "Link to N%u RECOVERED (heard again after %lu failures) - "
              "recomputing routes", from, (unsigned long)n.txFailed);
    s_pendingAnnounce = true;
    s_announceEvent   = RE_LINK_UP;
    s_announceA       = NODE_ID;
    s_announceB       = from;
    routeRecompute();
  }
}

void routeNoteBleRssi(uint8_t id, int8_t rssi) {
  if (id < 1 || id > NUM_NODES || id == NODE_ID) return;
  s_nbr[id].bleRssi = rssi;
  if (!s_nbr[id].known) s_nbr[id].known = true;
}

/* The failure detector.  Called from the MAC on every ACK outcome.           */
void routeReportTxResult(uint8_t peer, bool ok) {
  if (peer < 1 || peer > NUM_NODES) return;
  Neighbor &n = s_nbr[peer];

  n.txAttempts++;
  if (ok) {
    n.txAcked++;
    n.consecFail  = 0;
    n.lastHeardMs = millis();
    return;
  }

  n.txFailed++;
  n.consecFail++;

  if (n.linkUp && n.consecFail >= ROUTE_FAIL_LIMIT) {
    /* --- 1. LINK FAILURE --- 2. FAILURE DETECTION ------------------------- */
    n.linkUp = false;
    n.linkDownEvents++;
    s_rt.failuresDetected++;
    s_rt.lastFailureMs = millis();

    LOGE(TAG, "LINK FAILURE DETECTED: N%u did not acknowledge %u consecutive "
              "frames - marking the link DOWN", peer, n.consecFail);
    logCsv("LINKDOWN,%u,%u", peer, n.consecFail);

    /* --- 3. ROUTE UPDATE --- 4. ALTERNATE ROUTE SELECTION ------------------ */
    const uint8_t before = s_rt.nextHop;
    routeRecompute();

    if (s_rt.nextHop && s_rt.nextHop != before) {
      s_rt.failoverMs = millis() - s_rt.lastFailureMs;
      LOGW(TAG, "ALTERNATE ROUTE SELECTED: next hop N%u -> N%u "
                "(%u hops to sink, cost %u, failover %lu ms)",
           before, s_rt.nextHop, s_rt.hopsToSink, s_rt.cost,
           (unsigned long)s_rt.failoverMs);
    } else if (!s_rt.nextHop) {
      LOGE(TAG, "No alternate route to the sink is available - the node is "
                "isolated until a neighbour returns");
    }

    s_pendingAnnounce = true;
    s_announceEvent   = RE_LINK_DOWN;
    s_announceA       = NODE_ID;
    s_announceB       = peer;
  }
}

/* -------------------------------------------------------------------------- */

static float rssiNorm(int8_t rssi) {
  if (rssi == 0) return 0.35f;      /* unknown -> mildly pessimistic default  */
  if (rssi <= RSSI_NORM_MIN) return 0.0f;
  if (rssi >= RSSI_NORM_MAX) return 1.0f;
  return (float)(rssi - RSSI_NORM_MIN) / (float)(RSSI_NORM_MAX - RSSI_NORM_MIN);
}

static uint16_t computeCost(uint8_t id) {
  Neighbor &n = s_nbr[id];

  if (!n.known)  return ROUTE_COST_INF;
  if (!n.linkUp) return ROUTE_COST_INF;                       /* availability */
  /* Topology constraint: this pair is not a usable link in the deployment
   * we are modelling.  See ROUTE_LINK_ALLOWED in wsn_config.cpp.            */
  if (!routeLinkAllowed(NODE_ID, id)) return ROUTE_COST_INF;
  if (millis() - n.lastHeardMs > NEIGHBOR_TIMEOUT_MS) return ROUTE_COST_INF;
  if (n.hopsToSink == 0xFF) return ROUTE_COST_INF;            /* no path      */

  /* Prefer the Wi-Fi RSSI the sniffer measured; fall back to the BLE RSSI
   * from the localization phase when Wi-Fi RSSI is not yet available.  This
   * is the concrete point where the BLE half of the system informs the Wi-Fi
   * half.                                                                    */
  const int8_t rssi = n.wifiRssi ? n.wifiRssi : n.bleRssi;

  const float ackRate  = n.txAttempts ? (float)n.txAcked / (float)n.txAttempts : 1.0f;
  const float lossRate = n.txAttempts ? (float)n.txFailed / (float)n.txAttempts : 0.0f;

  float c = (float)ROUTE_W_HOP  * (float)(n.hopsToSink + 1)
          + (float)ROUTE_W_RSSI * (1.0f - rssiNorm(rssi))
          + (float)ROUTE_W_ACK  * (1.0f - ackRate)
          + (float)ROUTE_W_LOSS * lossRate;

  /* Prefer the cluster head: a sensor node reporting to its head is what
   * makes aggregation possible, and routing around the head would deliver
   * the same readings in three times as many packets.  It is a discount, not
   * a hard rule -- once the head's link degrades or fails, the arithmetic
   * lets an ordinary neighbour win.                                         */
  if (id == CLUSTER_HEAD_ID &&
      (NODE_ROLE == ROLE_SENSOR || NODE_ROLE == ROLE_SENSOR_RELAY)) {
    c -= (float)ROUTE_W_CH_BONUS;
  }

  if (c < 0.0f) c = 0.0f;
  if (c >= (float)ROUTE_COST_INF) return ROUTE_COST_INF - 1;
  return (uint16_t)c;
}

bool routeRecompute(void) {
  if (NODE_ID == SINK_ID) { s_rt.hopsToSink = 0; s_rt.cost = 0; return false; }

  uint8_t  best     = 0;
  uint16_t bestCost = ROUTE_COST_INF;

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID) continue;
    s_nbr[i].cost = computeCost(i);
    if (s_nbr[i].cost < bestCost) { bestCost = s_nbr[i].cost; best = i; }
  }

  /* Hysteresis.  Two links of nearly equal cost would otherwise swap on every
   * sweep -- 70 route changes in a few minutes, each one abandoning an
   * in-flight frame.  The incumbent keeps the route unless a challenger beats
   * it by a real margin.                                                    */
  const uint8_t prev = s_rt.nextHop;
  if (prev && prev != best && prev <= NUM_NODES &&
      s_nbr[prev].cost < ROUTE_COST_INF &&
      s_nbr[prev].cost <= (uint16_t)(bestCost + ROUTE_HYSTERESIS)) {
    best     = prev;
    bestCost = s_nbr[prev].cost;
  }

  s_rt.nextHop    = best;
  s_rt.cost       = bestCost;
  s_rt.hopsToSink = best ? (uint8_t)(s_nbr[best].hopsToSink + 1) : 0xFF;

  /* "Using an alternate route" means: not going through the cluster head,
   * which is the designed primary path for every sensor node.                */
  s_rt.usingAlternate = (best != 0 && best != CLUSTER_HEAD_ID &&
                         NODE_ROLE != ROLE_CLUSTER_HEAD);

  if (best != prev) {
    s_rt.previousNextHop = prev;
    s_rt.routeChanges++;
    s_rt.lastChangeMs = millis();

    LOGW(TAG, "ROUTE CHANGED: next hop N%u -> N%u  (%u hops to sink, cost %u)%s",
         prev, best, s_rt.hopsToSink, s_rt.cost,
         s_rt.usingAlternate ? "  [ALTERNATE PATH]" : "  [primary path]");
    logCsv("ROUTE,%u,%u,%u,%u", prev, best, s_rt.hopsToSink, s_rt.cost);

    s_pendingAnnounce = true;
    if (s_announceEvent == 0) {
      s_announceEvent = RE_ROUTE_CHANGED;
      s_announceA     = prev;
      s_announceB     = best;
    }
    return true;
  }
  return false;
}

/* -------------------------------------------------------------------------- */

uint8_t routeNextHop(uint8_t finalDst) {
  if (finalDst == NODE_ID) return 0;

  /* Traffic for the sink always follows the computed route.  Deliberately no
   * "destination is a neighbour, send it straight there" shortcut: on a bench
   * every node can hear the sink, so that shortcut would collapse the network
   * to one hop and neither multi-hop forwarding nor aggregation would ever
   * run.  The direct link is still chosen when the cost metric says it is
   * best and the topology permits it.                                       */
  if (finalDst == SINK_ID) return s_rt.nextHop;

  /* For any other destination, hand it over directly when that link is
   * permitted and healthy; otherwise fall back to the route toward the sink. */
  if (finalDst >= 1 && finalDst <= NUM_NODES && routeLinkAllowed(NODE_ID, finalDst)) {
    Neighbor &d = s_nbr[finalDst];
    if (d.known && d.linkUp && (millis() - d.lastHeardMs) <= NEIGHBOR_TIMEOUT_MS)
      return finalDst;
  }
  return s_rt.nextHop;
}

uint8_t  routeHopsToSink(void)      { return s_rt.hopsToSink; }
uint16_t routeOwnCost(void)         { return s_rt.cost; }
bool     routeIsUsingAlternate(void){ return s_rt.usingAlternate; }

void routeForceLinkState(uint8_t id, bool up) {
  if (id < 1 || id > NUM_NODES) return;

  macSimulateLinkFailure(id, !up);

  if (!up) {
    s_nbr[id].linkUp     = false;
    s_nbr[id].consecFail = ROUTE_FAIL_LIMIT;
    s_rt.failuresDetected++;
    s_rt.lastFailureMs = millis();
    LOGW(TAG, "Operator forced link to N%u DOWN", id);
  } else {
    s_nbr[id].linkUp      = true;
    s_nbr[id].consecFail  = 0;
    s_nbr[id].lastHeardMs = millis();
    LOGI(TAG, "Operator forced link to N%u UP", id);
  }
  routeRecompute();

  s_pendingAnnounce = true;
  s_announceEvent   = up ? RE_LINK_UP : RE_LINK_DOWN;
  s_announceA       = NODE_ID;
  s_announceB       = id;
}

/* -------------------------------------------------------------------------- */

void routeLoop(void) {
  static uint32_t lastSweep = 0;
  if (millis() - lastSweep < 500) return;
  lastSweep = millis();

  /* Silence-based failure detection, complementing the ACK-based one.  A node
   * that is powered off never fails an ACK if we never send it one, so the
   * timeout is what catches a dead neighbour we were not transmitting to.   */
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID || !s_nbr[i].known || !s_nbr[i].linkUp) continue;
    if (millis() - s_nbr[i].lastHeardMs > NEIGHBOR_TIMEOUT_MS) {
      s_nbr[i].linkUp = false;
      s_nbr[i].linkDownEvents++;
      s_rt.failuresDetected++;
      s_rt.lastFailureMs = millis();

      LOGE(TAG, "LINK FAILURE DETECTED: N%u silent for %lu ms - marking DOWN",
           i, (unsigned long)(millis() - s_nbr[i].lastHeardMs));
      logCsv("LINKDOWN,%u,0", i);

      const uint8_t before = s_rt.nextHop;
      routeRecompute();
      if (s_rt.nextHop && s_rt.nextHop != before) {
        s_rt.failoverMs = millis() - s_rt.lastFailureMs;
        LOGW(TAG, "ALTERNATE ROUTE SELECTED: N%u -> N%u (failover %lu ms)",
             before, s_rt.nextHop, (unsigned long)s_rt.failoverMs);
      }

      s_pendingAnnounce = true;
      s_announceEvent   = RE_LINK_DOWN;
      s_announceA       = NODE_ID;
      s_announceB       = i;
    }
  }

  /* Refresh costs even when nothing failed: RSSI and ACK rates drift, and the
   * route should follow them.  This is what makes the routing *adaptive*
   * rather than only reactive to outright failure.                           */
  routeRecompute();
}

void routeFillRoutePayload(RoutePayload *p, uint8_t event, uint8_t a, uint8_t b) {
  memset(p, 0, sizeof(*p));
  p->originator = NODE_ID;
  p->event      = event;
  p->peerA      = a;
  p->peerB      = b;
  p->newNextHop = s_rt.nextHop;
  p->hopsToSink = s_rt.hopsToSink;
  p->cost       = s_rt.cost;
}

/* Consumed by the node sketches: "is there a route event worth broadcasting?" */
bool routeTakePendingAnnounce(uint8_t *event, uint8_t *a, uint8_t *b) {
  if (!s_pendingAnnounce) return false;
  *event = s_announceEvent;
  *a     = s_announceA;
  *b     = s_announceB;
  s_pendingAnnounce = false;
  s_announceEvent   = 0;
  return true;
}

const Neighbor   *routeNeighbors(void) { return s_nbr; }
const RouteState *routeState(void)     { return &s_rt; }

void routePrintTable(void) {
  Serial.println();
  Serial.println("  --- NEIGHBOUR TABLE ----------------------------------------");
  Serial.println("   node  state  wifi  ble   hops  tx    ack   loss%   cost");
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID) continue;
    const Neighbor &n = s_nbr[i];
    if (!n.known) {
      Serial.printf("   N%-4u  --     --    --    --    --    --    --      --\r\n", i);
      continue;
    }
    const float loss = n.txAttempts ? 100.0f * n.txFailed / n.txAttempts : 0.0f;
    char cost[10];
    if (n.cost >= ROUTE_COST_INF) strcpy(cost, "INF");
    else snprintf(cost, sizeof(cost), "%u", n.cost);

    Serial.printf("   N%-4u  %-5s  %-5d %-5d %-5s %-5lu %-5lu %-7.1f %s\r\n",
                  i, n.linkUp ? "UP" : "DOWN", n.wifiRssi, n.bleRssi,
                  n.hopsToSink == 0xFF ? "?" : String(n.hopsToSink).c_str(),
                  (unsigned long)n.txAttempts, (unsigned long)n.txAcked,
                  loss, cost);
  }
  Serial.println("  --- ROUTE --------------------------------------------------");
  if (NODE_ID == SINK_ID) {
    Serial.println("   This node IS the sink - it is the destination.");
  } else if (s_rt.nextHop) {
    Serial.printf("   Next hop       : N%u   (%u hops to sink, cost %u)\r\n",
                  s_rt.nextHop, s_rt.hopsToSink, s_rt.cost);
    Serial.printf("   Path           : N%u -> N%u%s -> N%u\r\n",
                  NODE_ID, s_rt.nextHop,
                  (s_rt.hopsToSink > 1) ? " -> ..." : "", SINK_ID);
    Serial.printf("   Using          : %s\r\n",
                  s_rt.usingAlternate ? "ALTERNATE path" : "primary path");
  } else {
    Serial.println("   Next hop       : NONE - no route to the sink");
  }
  Serial.printf("   Route changes  : %lu   failures detected: %lu   "
                "recoveries: %lu\r\n",
                (unsigned long)s_rt.routeChanges,
                (unsigned long)s_rt.failuresDetected,
                (unsigned long)s_rt.recoveries);
  if (s_rt.failoverMs)
    Serial.printf("   Last failover  : %lu ms (detection to alternate route)\r\n",
                  (unsigned long)s_rt.failoverMs);
  Serial.println();
}
