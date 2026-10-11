/* =============================================================================
 *  wsn_route.h  --  Neighbour table and adaptive multi-hop routing.
 *
 *  Route cost combines all five parameters the lab guidelines list:
 *
 *      cost = W_HOP  * (hopsToSink + 1)          hop count
 *           + W_RSSI * (1 - rssiNorm)            RSSI / link quality
 *           + W_ACK  * (1 - ackRate)             successful ACK rate
 *           + W_LOSS * lossRate                  packet loss
 *           = ROUTE_COST_INF  if the link is down   link availability
 *
 *  rssiNorm maps -95..-35 dBm onto 0..1.  The RSSI input prefers the Wi-Fi
 *  RSSI measured by the MAC sniffer and falls back to the BLE RSSI from the
 *  localization phase -- so the BLE half of the system feeds the Wi-Fi half,
 *  which is the point of building a hybrid network rather than two separate
 *  ones.
 *
 *  FAILURE AND RECOVERY
 *      link failure      a peer stops acknowledging (power-off, or `fail <id>`)
 *      detection         ROUTE_FAIL_LIMIT consecutive ACK losses, or
 *                        NEIGHBOR_TIMEOUT_MS of silence
 *      route update      cost -> infinity, ROUTE_UPDATE broadcast
 *      alternate route   the next-lowest-cost neighbour with a path to the sink
 *      delivery          traffic continues over the new path, and the sink
 *                        shows the changed hop list
 *      recovery          the peer is heard again, cost is recomputed, and the
 *                        better original route wins on its own merits
 * ========================================================================== */

#ifndef WSN_ROUTE_H
#define WSN_ROUTE_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef struct {
  bool     known;           /* heard at least once                           */
  bool     linkUp;
  int8_t   wifiRssi;        /* from the MAC sniffer                          */
  int8_t   bleRssi;         /* from the BLE localization phase               */
  uint8_t  hopsToSink;      /* advertised by the neighbour                   */
  uint8_t  role;
  uint16_t cost;            /* computed route cost through this neighbour    */
  uint32_t lastHeardMs;
  uint32_t txAttempts;      /* unicasts sent to this neighbour               */
  uint32_t txAcked;
  uint32_t txFailed;
  uint16_t consecFail;      /* consecutive ACK losses -- the failure detector */
  uint32_t rxFrames;        /* frames received from this neighbour           */
  uint32_t linkDownEvents;
} Neighbor;

typedef struct {
  uint8_t  nextHop;             /* current next hop toward the sink          */
  uint8_t  hopsToSink;
  uint16_t cost;
  uint8_t  previousNextHop;
  uint32_t routeChanges;
  uint32_t failuresDetected;
  uint32_t recoveries;
  uint32_t lastChangeMs;
  uint32_t lastFailureMs;
  uint32_t failoverMs;          /* detection -> alternate route, in ms       */
  bool     usingAlternate;      /* true when not on the preferred path       */
} RouteState;

void routeBegin(void);
void routeLoop(void);

/* Feed the table.                                                            */
void routeNoteRx(uint8_t from, int8_t rssi, uint8_t hopsToSink, uint8_t role);
void routeNoteBleRssi(uint8_t id, int8_t rssi);
/* Called by the MAC on every ACK outcome -- this is the failure detector's
 * primary input.                                                             */
void routeReportTxResult(uint8_t peer, bool ok);

/* Current best next hop toward `finalDst`, or 0 if none is reachable.        */
uint8_t  routeNextHop(uint8_t finalDst);
uint8_t  routeHopsToSink(void);
uint16_t routeOwnCost(void);
bool     routeIsUsingAlternate(void);

/* Recompute costs and re-select; returns true if the next hop changed.       */
bool routeRecompute(void);

/* Force a neighbour down / up (the `fail` and `recover` demonstrations).     */
void routeForceLinkState(uint8_t id, bool up);

/* Pop a route event that should be broadcast as an FT_ROUTE_UPDATE.  Returns
 * false when there is nothing to announce.                                   */
bool routeTakePendingAnnounce(uint8_t *event, uint8_t *a, uint8_t *b);

const Neighbor   *routeNeighbors(void);
const RouteState *routeState(void);
void routePrintTable(void);
void routeFillRoutePayload(RoutePayload *p, uint8_t event, uint8_t a, uint8_t b);

#endif /* WSN_ROUTE_H */
