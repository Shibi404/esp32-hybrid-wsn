/* =============================================================================
 *  NODE 4  --  SINK / GATEWAY       23CSE341 WSN Lab Evaluation 2
 * =============================================================================
 *
 *  ROLE      Sink and gateway: collects everything, computes the performance
 *            metrics, and hosts the real-time dashboard.  Also a permanent BLE
 *            anchor.
 *  SENSOR    HC-SR501  (PIR motion)
 *  WIRING    PIR OUT -> GPIO 27   (3.3 V TTL output, safe to connect directly)
 *            PIR VCC -> 5V / VIN  (the HC-SR501 needs 4.5-20 V)
 *            PIR GND -> GND
 *            Set the trigger jumper to H (repeatable) and the time-delay
 *            trimmer to minimum, so motion events are responsive in the demo.
 *
 *  DASHBOARD
 *            Join Wi-Fi  "WSN_SINK"  /  password "wsnlab2024"
 *            Browse to   http://192.168.4.1
 *
 *            The SoftAP runs on the SAME channel as ESP-NOW (channel 1).  They
 *            share one radio, so a mismatched channel silently breaks one of
 *            them -- that is the classic failure mode for this design.
 *
 *  WHY THIS NODE NEVER SLEEPS AND NEVER SCANS
 *            A gateway is mains-powered; energy is only scarce at the leaves.
 *            It stays in the Wi-Fi phase permanently so the dashboard is always
 *            reachable, and it only ADVERTISES over BLE (never scans), because
 *            a blocking 4 s scan would stall the page it has to serve.  It is
 *            still a full localization anchor -- advertising is all an anchor
 *            has to do.
 * ========================================================================== */

#include "wsn_node.h"
#include "wsn_log.h"
#include "wsn_mac.h"
#include "wsn_tdma.h"
#include "wsn_route.h"
#include "wsn_sensors.h"
#include "wsn_metrics.h"
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

#include "dashboard_html.h"

#define TAG "SINK"

static WebServer server(80);
static bool      s_apUp = false;

/* -----------------------------------------------------------------------------
 *  JSON is hand-rolled rather than pulled from a library.  It keeps the
 *  dependency list to the two Adafruit sensor libraries, which removes a whole
 *  class of version-mismatch failures on demo day, and the payload is small
 *  and completely fixed in shape.
 * -------------------------------------------------------------------------- */

static void jsonNum(String &s, const char *key, float v, uint8_t dp = 2) {
  s += '"'; s += key; s += "\":";
  if (isfinite(v)) {
    /* snprintf rather than String(v, dp): the String(float, uint8_t)
     * constructor is ambiguous between the float and double overloads.      */
    char buf[24];
    snprintf(buf, sizeof(buf), "%.*f", (int)dp, (double)v);
    s += buf;
  } else {
    s += "null";          /* an invalid reading must never become 0 */
  }
  s += ',';
}

static void jsonInt(String &s, const char *key, long v) {
  s += '"'; s += key; s += "\":"; s += v; s += ',';
}

static void jsonStr(String &s, const char *key, const char *v) {
  s += '"'; s += key; s += "\":\""; s += v; s += "\",";
}

static void jsonBool(String &s, const char *key, bool v) {
  s += '"'; s += key; s += "\":"; s += (v ? "true" : "false"); s += ',';
}

/* Drop the trailing comma an object accumulated, then close it.              */
static void jsonClose(String &s, char brace = '}') {
  if (s.length() && s[s.length() - 1] == ',') s.remove(s.length() - 1);
  s += brace;
}

static String buildState(void) {
  String j;
  j.reserve(4096);
  j += '{';

  /* ---- node status ------------------------------------------------------- */
  jsonInt (j, "uptime",   wsnNodeUptimeS());
  jsonStr (j, "phase",    wsnNodePhaseName(wsnNodePhase()));
  jsonInt (j, "heap",     ESP.getFreeHeap());
  jsonInt (j, "slot",     tdmaCurrentSlot());
  jsonInt (j, "superframe", tdmaSuperframeIndex());
  jsonBool(j, "synced",   tdmaIsSynced());

  j += "\"nodes\":[";
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    const NodeView *v = wsnNodeView(i);
    const Neighbor *n = &routeNeighbors()[i];
    const bool self   = (i == NODE_ID);

    j += '{';
    jsonInt (j, "id", i);
    jsonStr (j, "name", NODE_NAME[i]);
    jsonBool(j, "self", self);
    jsonBool(j, "online", self || (v->seen &&
             (millis() - v->lastHeardMs) < NEIGHBOR_TIMEOUT_MS));
    jsonInt (j, "age", v->seen ? (long)((millis() - v->lastHeardMs) / 1000) : -1);
    jsonInt (j, "hops", self ? 0 : (v->hopsToSink == 0xFF ? -1 : v->hopsToSink));
    jsonInt (j, "next", v->nextHop);
    jsonInt (j, "rssi", self ? 0 : v->rssi);
    jsonInt (j, "bleRssi", self ? 0 : n->bleRssi);
    jsonInt (j, "retx", v->retx);
    jsonInt (j, "alarm", self ? sensorsGet()->alarm : v->alarm);
    jsonBool(j, "linkUp", self ? true : n->linkUp);
    jsonStr (j, "path", v->lastPath);

    /* sensor data */
    if (self) {
      const SensorData *d = sensorsGet();
      jsonNum (j, "temp", (d->sensorMask & SENS_TEMP) ? d->temperature : NAN);
      jsonNum (j, "hum",  (d->sensorMask & SENS_HUM)  ? d->humidity    : NAN);
      jsonNum (j, "gas",  (d->sensorMask & SENS_GAS)  ? d->gasPpm      : NAN, 0);
      jsonInt (j, "motion", (d->sensorMask & SENS_MOTION) ? d->motion : -1);
      jsonBool(j, "warming", d->warming);
    } else if (v->hasData) {
      jsonNum (j, "temp", (v->last.sensorMask & SENS_TEMP) ? v->last.temperature : NAN);
      jsonNum (j, "hum",  (v->last.sensorMask & SENS_HUM)  ? v->last.humidity    : NAN);
      jsonNum (j, "gas",  (v->last.sensorMask & SENS_GAS)  ? v->last.gasPpm      : NAN, 0);
      jsonInt (j, "motion", (v->last.sensorMask & SENS_MOTION) ? v->last.motion : -1);
      jsonBool(j, "warming", (v->last.flags & DFLAG_WARMING) != 0);
    } else {
      j += "\"temp\":null,\"hum\":null,\"gas\":null,\"motion\":-1,\"warming\":false,";
    }

    /* per-source delivery */
    const SrcMetrics *m = metricsForSource(i);
    jsonNum (j, "pdr", m->seen ? metricsPdr(i) * 100.0f : NAN);
    jsonInt (j, "rx",  m->rxPackets);
    jsonInt (j, "lost", m->lost);
    jsonClose(j);
    if (i < NUM_NODES) j += ',';
  }
  j += "],";

  /* ---- BLE localization --------------------------------------------------- */
  j += "\"loc\":";
  const LocPayload *l = wsnNodeLastLocalization();
  if (l) {
    j += '{';
    jsonInt(j, "node", l->nodeId);
    jsonInt(j, "anchors", l->nAnchors);
    jsonInt(j, "nearest", l->nearestId);
    jsonInt(j, "nearestRssi", l->nearestRssi);
    jsonNum(j, "estX", l->estX);
    jsonNum(j, "estY", l->estY);
    jsonNum(j, "trueX", l->trueX);
    jsonNum(j, "trueY", l->trueY);
    jsonNum(j, "error", l->errorM);
    jsonNum(j, "rmse", l->rmseM);
    j += "\"anchorList\":[";
    for (uint8_t i = 1; i <= NUM_NODES; i++) {
      j += '{';
      jsonInt(j, "id", i);
      jsonInt(j, "rssi", l->rssi[i]);
      jsonNum(j, "dist", l->distCm[i] / 100.0f);
      jsonNum(j, "x", NODE_POS[i].x);
      jsonNum(j, "y", NODE_POS[i].y);
      jsonBool(j, "anchor", NODE_IS_ANCHOR[i]);
      jsonClose(j);
      if (i < NUM_NODES) j += ',';
    }
    j += ']';
    jsonClose(j);
  } else {
    j += "null";
  }
  j += ',';

  /* ---- aggregated data ---------------------------------------------------- */
  j += "\"agg\":";
  const AggPayload *a = wsnNodeLastAggregate();
  if (a) {
    j += '{';
    jsonInt(j, "samples", a->nSamples);
    jsonInt(j, "srcMask", a->srcMask);
    jsonInt(j, "alarms",  a->alarmOr);
    jsonNum(j, "tMin", a->tMin); jsonNum(j, "tAvg", a->tAvg); jsonNum(j, "tMax", a->tMax);
    jsonNum(j, "hMin", a->hMin); jsonNum(j, "hAvg", a->hAvg); jsonNum(j, "hMax", a->hMax);
    jsonNum(j, "gMin", a->gMin, 0); jsonNum(j, "gAvg", a->gAvg, 0); jsonNum(j, "gMax", a->gMax, 0);
    jsonInt(j, "rawPackets", a->rawPackets);
    jsonInt(j, "rawBytes",   a->rawBytes);
    jsonInt(j, "aggBytes",   a->aggBytes);
    jsonClose(j);
  } else {
    j += "null";
  }
  j += ',';

  /* ---- MAC activity ------------------------------------------------------- */
  const MacStats *ms = macStats();
  j += "\"mac\":{";
  jsonStr (j, "state", macGetState() == MAC_IDLE    ? "IDLE"
                     : macGetState() == MAC_SENSE   ? "SENSE"
                     : macGetState() == MAC_BACKOFF ? "BACKOFF"
                     : macGetState() == MAC_TX      ? "TX" : "WAIT_ACK");
  jsonBool(j, "busy", macIsChannelBusy());
  jsonInt (j, "cw", macGetCw());
  jsonInt (j, "lastBackoff", ms->lastBackoff);
  jsonInt (j, "txAttempts", ms->txAttempts);
  jsonInt (j, "txSuccess",  ms->txSuccess);
  jsonInt (j, "retx",       ms->retransmissions);
  jsonInt (j, "ackTimeouts", ms->ackTimeouts);
  jsonInt (j, "dropped",    ms->txDropped);
  jsonInt (j, "ccaDeferrals", ms->ccaBusyDeferrals);
  jsonInt (j, "freezes",    ms->backoffFreezes);
  jsonInt (j, "rxFrames",   ms->rxFrames);
  jsonInt (j, "badCrc",     ms->rxBadCrc);
  jsonNum (j, "ackRate",    macAckRate() * 100.0f);
  jsonInt (j, "ackRtt",     macAvgAckRttMs());
  jsonClose(j);
  j += ',';

  /* ---- schedule ----------------------------------------------------------- */
  const Schedule *sc = tdmaSchedule();
  j += "\"sched\":{";
  jsonInt(j, "version", sc->version);
  jsonStr(j, "reason", wsnSchedReasonName(sc->reason));
  jsonInt(j, "reasonNode", sc->reasonNode);
  jsonInt(j, "updates", tdmaStats()->scheduleUpdates);
  jsonNum(j, "duty", tdmaDutyCyclePct(), 1);
  j += "\"slots\":[";
  for (uint8_t i = 0; i < TDMA_NUM_SLOTS; i++) {
    j += '{';
    jsonInt(j, "i", i);
    jsonStr(j, "kind", tdmaSlotKindName(sc->kind[i]));
    jsonInt(j, "owner", sc->owner[i] == BROADCAST_ID ? 0xFF : sc->owner[i]);
    jsonInt(j, "owner2", sc->owner2[i]);
    jsonClose(j);
    if (i + 1 < TDMA_NUM_SLOTS) j += ',';
  }
  j += ']';
  jsonClose(j);
  j += ',';

  /* ---- routing ------------------------------------------------------------ */
  /* Report what the NETWORK did, not what the sink did.  The sink is the
   * destination: its own next hop never changes, so its RouteState counters
   * are permanently zero and reporting them made a re-routing network look
   * completely static.  These come from the ROUTE_UPDATE frames other nodes
   * broadcast when they detect a failure or switch route.                  */
  j += "\"route\":{";
  jsonInt(j, "events", wsnNodeRouteEventsSeen());
  jsonInt(j, "linkDowns", wsnNodeLinkDownsSeen());
  jsonClose(j);
  j += ',';

  /* ---- performance -------------------------------------------------------- */
  j += "\"perf\":{";
  jsonNum(j, "pdr", metricsPdrOverall() * 100.0f);
  jsonInt(j, "rx", metricsRxOverall());
  jsonInt(j, "lost", metricsLostOverall());
  jsonInt(j, "delayAvg", metricsAvgDelayMs());
  jsonInt(j, "delayMax", metricsMaxDelayMs());
  jsonNum(j, "throughput", metricsThroughputBps(), 1);
  jsonInt(j, "retx", metricsRetxOverall());
  jsonNum(j, "reduction", metricsDataReductionPct());
  jsonNum(j, "locError", l ? l->errorM : NAN);
  jsonNum(j, "locRmse",  l ? l->rmseM  : NAN);
  jsonClose(j);

  jsonClose(j);
  return j;
}

/* -------------------------------------------------------------------------- */

static void handleRoot(void) {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleState(void) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildState());
}

static void handleCmd(void) {
  const String c = server.arg("c");
  const int    n = server.arg("n").toInt();

  if      (c == "fail")        wsnNodeSendCommand(BROADCAST_ID, CMD_FAIL_LINK,    n);
  else if (c == "recover")     wsnNodeSendCommand(BROADCAST_ID, CMD_RECOVER_LINK, n);
  else if (c == "collide")     wsnNodeSendCommand(BROADCAST_ID, CMD_COLLIDE,      0);
  else if (c == "collidehard") wsnNodeSendCommand(BROADCAST_ID, CMD_COLLIDE,      1);
  else if (c == "reset")       wsnNodeSendCommand(BROADCAST_ID, CMD_RESET_STATS,  0);
  else { server.send(400, "text/plain", "unknown command"); return; }

  LOGI(TAG, "Dashboard command: %s %d", c.c_str(), n);
  server.send(200, "text/plain", "ok");
}

/* -------------------------------------------------------------------------- */

static void onWifiUp(void) {
  if (s_apUp) return;

  /* The SoftAP MUST be on the ESP-NOW channel.  One radio serves both; a
   * mismatch silently kills ESP-NOW reception while the AP still looks fine. */
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                    IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS, WSN_WIFI_CHANNEL, 0, 4)) {
    LOGE(TAG, "SoftAP failed to start - the dashboard will be unreachable");
    return;
  }

  server.on("/",           handleRoot);
  server.on("/api/state",  handleState);
  server.on("/api/cmd",    handleCmd);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  s_apUp = true;

  logSection("DASHBOARD READY");
  LOGI(TAG, "Join Wi-Fi  SSID \"%s\"  password \"%s\"  (channel %d)",
       SOFTAP_SSID, SOFTAP_PASS, WSN_WIFI_CHANNEL);
  LOGI(TAG, "Then browse to  http://%s", WiFi.softAPIP().toString().c_str());
}

static void onLoop(void) {
  if (!s_apUp) return;

  server.handleClient();

  /* Serving a page blocks for long enough to miss an ACK deadline, and the
   * ACKs this node owes are only transmitted from macLoop().  Run it again
   * the moment the handler returns, rather than waiting for the next pass
   * through wsnNodeLoop(), so a browser poll cannot cost the cluster head a
   * retransmission.                                                         */
  macLoop();
}

/* -------------------------------------------------------------------------- */

void setup() {
  static const NodeHooks hooks = { onWifiUp, nullptr, onLoop, nullptr };
  wsnNodeSetup(NODE_N4, ROLE_SINK, &hooks);
}

void loop() {
  wsnNodeLoop();
}
