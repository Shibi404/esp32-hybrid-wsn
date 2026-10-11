/* =============================================================================
 *  wsn_cli.cpp
 * ========================================================================== */

#include "wsn_cli.h"
#include "wsn_config.h"
#include "wsn_log.h"
#include "wsn_node.h"
#include "wsn_ble.h"
#include "wsn_mac.h"
#include "wsn_tdma.h"
#include "wsn_route.h"
#include "wsn_sensors.h"
#include "wsn_metrics.h"
#include "wsn_aggregate.h"
#include <WiFi.h>

#define TAG "CLI"

static char   s_line[96];
static uint8_t s_len = 0;

void cliBegin(void) {
  s_len = 0;
  Serial.println();
  Serial.println("Type `help` for the command list.");
}

void cliPrintHelp(void) {
  Serial.println();
  Serial.println("  ================= COMMANDS =================================");
  Serial.println("   help              this list");
  Serial.println("   status            phase, uptime, role, heap, network view");
  Serial.println("   stats             MAC + TDMA + routing + performance");
  Serial.println("   mac               CSMA/CA counters in detail");
  Serial.println("   nbr               neighbour table (RSSI, ACK rate, cost)");
  Serial.println("   route             next hop, hop count, path, failovers");
  Serial.println("   sched             live slot map and schedule version");
  Serial.println("   sensors           raw + converted readings, health, alarms");
  Serial.println("   agg               aggregation window and data reduction");
  Serial.println("   perf              performance metrics (sink has them all)");
  Serial.println("   loc               BLE localization result");
  Serial.println("  ---- calibration -------------------------------------------");
  Serial.println("   calib             measure MQ R0 in clean air (saved to NVS)");
  Serial.println("   calib1m <id>      record BLE RSSI at exactly 1 m from node id");
  Serial.println("   pathloss <n>      set the path-loss exponent (1.5 - 4.5)");
  Serial.println("   settrue <x> <y>   set this node's true position, in metres");
  Serial.println("  ---- demonstrations ----------------------------------------");
  Serial.println("   fail <id>         make node <id> play dead; its neighbours");
  Serial.println("                     must DETECT the failure themselves");
  Serial.println("   recover <id>      bring node <id> back");
  Serial.println("   faillink <id>     force just THIS node's link to <id> down");
  Serial.println("   uplink <id>       restore it");
  Serial.println("   collide           two nodes transmit in one slot (CSMA/CA");
  Serial.println("                     avoids the collision)");
  Serial.println("   collide hard      bypass carrier sense to force a real");
  Serial.println("                     collision, then show the retry recovery");
  Serial.println("   reset             clear MAC and performance counters");
  Serial.println("  ---- output ------------------------------------------------");
  Serial.println("   csv on | off      toggle the machine-readable CSV stream");
  Serial.println("   debug on | off    verbose logging");
  Serial.println("   reboot            restart this node");
  Serial.println("  ============================================================");
  Serial.println();
}

/* -------------------------------------------------------------------------- */

static void printStatus(void) {
  Serial.println();
  Serial.printf("  Node        : %s (id %u)  role %s\r\n",
                NODE_NAME[NODE_ID], NODE_ID, wsnRoleName(NODE_ROLE));
  Serial.printf("  Phase       : %s\r\n", wsnNodePhaseName(wsnNodePhase()));
  Serial.printf("  Uptime      : %lu s     free heap %lu B\r\n",
                (unsigned long)wsnNodeUptimeS(), (unsigned long)ESP.getFreeHeap());
  Serial.printf("  Sensor      : %s%s\r\n", sensorsName(),
                sensorsWarming() ? "  [WARMING UP]" : "");
  Serial.printf("  Simulated   : %s\r\n",
                wsnNodeIsSimulatedDown() ? "*** PLAYING DEAD ***" : "alive");
  Serial.printf("  Reports sent: %lu\r\n", (unsigned long)wsnNodeReportsSent());
  if (NODE_ROLE == ROLE_SINK)
    Serial.printf("  Dashboard   : http://%s   (SSID %s)\r\n",
                  WiFi.softAPIP().toString().c_str(), SOFTAP_SSID);

  Serial.println();
  Serial.println("  --- NETWORK VIEW -------------------------------------------");
  Serial.println("   node  last    role           hops  next  rssi  retx  alarm  path");
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID) continue;
    const NodeView *v = wsnNodeView(i);
    if (!v->seen) { Serial.printf("   N%-4u  never\r\n", i); continue; }
    Serial.printf("   N%-4u  %-6lu  %-14s %-5s %-5u %-5d %-5u 0x%02X   %s\r\n",
                  i, (unsigned long)((millis() - v->lastHeardMs) / 1000),
                  wsnRoleName((NodeRole)v->role),
                  v->hopsToSink == 0xFF ? "?" : String(v->hopsToSink).c_str(),
                  v->nextHop, v->rssi, v->retx, v->alarm, v->lastPath);
  }
  Serial.println();
}

static void printPerf(void) {
  metricsPrint();
  if (NODE_ROLE != ROLE_SINK)
    Serial.println("  (The complete picture lives on the sink, N4 - it sees "
                   "every delivered packet.)\r\n");

  const LocPayload *l = wsnNodeLastLocalization();
  if (l) {
    Serial.printf("    6. Localization error    : %.2f m  (RMSE %.2f m, "
                  "node N%u, %u anchors)\r\n",
                  l->errorM, l->rmseM, l->nodeId, l->nAnchors);
    Serial.println();
  }
}

/* -------------------------------------------------------------------------- */

static void execute(char *line) {
  /* trim */
  while (*line == ' ') line++;
  char *end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\r')) *--end = '\0';
  if (!*line) return;

  Serial.printf("\r\n> %s\r\n", line);

  char *cmd = strtok(line, " ");
  char *a1  = strtok(nullptr, " ");
  char *a2  = strtok(nullptr, " ");

  if      (!strcmp(cmd, "help"))    cliPrintHelp();
  else if (!strcmp(cmd, "status"))  printStatus();
  else if (!strcmp(cmd, "mac"))     macPrintStats();
  else if (!strcmp(cmd, "nbr") ||
           !strcmp(cmd, "route"))   routePrintTable();
  else if (!strcmp(cmd, "sched"))   { tdmaPrintSchedule(); tdmaPrintStats(); }
  else if (!strcmp(cmd, "sensors")) sensorsPrintStatus();
  else if (!strcmp(cmd, "agg"))     aggPrint();
  else if (!strcmp(cmd, "perf"))    printPerf();
  else if (!strcmp(cmd, "loc"))     blePrintStatus();

  else if (!strcmp(cmd, "stats")) {
    printStatus();
    macPrintStats();
    tdmaPrintStats();
    routePrintTable();
    printPerf();
  }

  else if (!strcmp(cmd, "calib")) sensorsCalibrate();

  else if (!strcmp(cmd, "calib1m")) {
    if (!a1) { Serial.println("  usage: calib1m <node id>"); return; }
    bleCalibrate1m((uint8_t)atoi(a1));
  }

  else if (!strcmp(cmd, "pathloss")) {
    if (!a1) { Serial.printf("  current path-loss exponent: %.2f\r\n",
                             bleGetPathLossN()); return; }
    bleSetPathLossN(atof(a1));
  }

  else if (!strcmp(cmd, "settrue")) {
    if (!a1 || !a2) { Serial.println("  usage: settrue <x> <y>   (metres)"); return; }
    NODE_POS[NODE_ID].x = atof(a1);
    NODE_POS[NODE_ID].y = atof(a2);
    Serial.printf("  Ground truth for %s set to (%.2f, %.2f) m. The next "
                  "localization round will be scored against it.\r\n",
                  NODE_NAME[NODE_ID], NODE_POS[NODE_ID].x, NODE_POS[NODE_ID].y);
  }

  else if (!strcmp(cmd, "fail")) {
    if (!a1) { Serial.println("  usage: fail <node id>"); return; }
    wsnNodeSendCommand(BROADCAST_ID, CMD_FAIL_LINK, atoi(a1));
    Serial.printf("  Node N%d will stop responding. Its neighbours are NOT told - "
                  "watch them detect the failure through ACK timeouts, then "
                  "re-route.\r\n", atoi(a1));
  }

  else if (!strcmp(cmd, "recover")) {
    if (!a1) { Serial.println("  usage: recover <node id>"); return; }
    wsnNodeSendCommand(BROADCAST_ID, CMD_RECOVER_LINK, atoi(a1));
  }

  else if (!strcmp(cmd, "faillink")) {
    if (!a1) { Serial.println("  usage: faillink <node id>"); return; }
    routeForceLinkState((uint8_t)atoi(a1), false);
    Serial.printf("  This node will no longer route via N%d. Use it to steer a "
                  "specific alternate path; `fail <id>` is the one that makes "
                  "neighbours detect a failure for themselves.\r\n", atoi(a1));
  }

  else if (!strcmp(cmd, "uplink")) {
    if (!a1) { Serial.println("  usage: uplink <node id>"); return; }
    routeForceLinkState((uint8_t)atoi(a1), true);
  }

  else if (!strcmp(cmd, "collide")) {
    const bool hard = (a1 && !strcmp(a1, "hard"));
    wsnNodeSendCommand(BROADCAST_ID, CMD_COLLIDE, hard ? 1 : 0);
    Serial.println(hard
      ? "  Forcing a real collision: carrier sense is bypassed, so expect an\r\n"
        "  ACK timeout, the contention window to double, and a successful\r\n"
        "  retransmission."
      : "  N1 and N2 will transmit in the same slot. Expect both to sense the\r\n"
        "  medium, draw different backoffs, and one to freeze while the other\r\n"
        "  transmits - a collision avoided rather than suffered.");
  }

  else if (!strcmp(cmd, "reset")) {
    wsnNodeSendCommand(BROADCAST_ID, CMD_RESET_STATS, 0);
  }

  else if (!strcmp(cmd, "csv")) {
    logSetCsv(!a1 || strcmp(a1, "off") != 0);
    Serial.printf("  CSV stream %s\r\n", logCsvEnabled() ? "ON" : "OFF");
  }

  else if (!strcmp(cmd, "debug")) {
    const bool on = (a1 && !strcmp(a1, "on"));
    logSetLevel(on ? LVL_DEBUG : LVL_INFO);
    Serial.printf("  Log level: %s\r\n", on ? "DEBUG" : "INFO");
  }

  else if (!strcmp(cmd, "reboot")) {
    Serial.println("  Rebooting...");
    delay(100);
    ESP.restart();
  }

  else {
    Serial.printf("  Unknown command \"%s\". Type `help`.\r\n", cmd);
  }
}

void cliLoop(void) {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_len) { s_line[s_len] = '\0'; execute(s_line); s_len = 0; }
    } else if (c == 8 || c == 127) {          /* backspace */
      if (s_len) s_len--;
    } else if (s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = c;
    }
  }
}
