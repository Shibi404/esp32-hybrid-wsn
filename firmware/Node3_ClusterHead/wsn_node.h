/* =============================================================================
 *  wsn_node.h  --  The shared node runtime.
 *
 *  All four sketches are thin: they set NODE_ID / NODE_ROLE, call
 *  wsnNodeSetup() and wsnNodeLoop(), and supply an optional hook for the parts
 *  that are genuinely role-specific (the sink's dashboard, for instance).
 *  Everything else -- the phase machine, the slot machine, the frame handling
 *  -- lives here, so a protocol fix lands on all four nodes at once instead of
 *  being copied three times and then diverging.
 *
 *  PHASE MACHINE (this is the demonstration flow from the lab guidelines)
 *
 *      BOOT
 *        |
 *        v
 *      PHASE_BLE        advertise, scan, RSSI, distance, trilateration,
 *        |              localization error            [~6 s]
 *        v
 *      PHASE_WIFI_INIT  BLE down, Wi-Fi up, ESP-NOW peers, (sink: SoftAP)
 *        |
 *        v
 *      PHASE_WIFI       beacon/schedule -> CSMA/CA -> TDMA transmission ->
 *        |              aggregation -> multi-hop -> sink -> metrics -> sleep
 *        |              [~30 s, then back to PHASE_BLE to refresh localization]
 *        v
 *      (repeat)
 * ========================================================================== */

#ifndef WSN_NODE_H
#define WSN_NODE_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef enum : uint8_t {
  PHASE_BOOT = 0,
  PHASE_BLE,
  PHASE_WIFI_INIT,
  PHASE_WIFI
} NodePhase;

/* Role-specific hooks.  Any of them may be null.                             */
typedef struct {
  void (*onWifiUp)(void);                       /* after ESP-NOW is ready     */
  void (*onWifiDown)(void);                     /* before BLE takes the radio */
  void (*onLoop)(void);                         /* every pass, in PHASE_WIFI  */
  void (*onFrame)(const WsnHeader *h, const uint8_t *payload,
                  size_t payloadLen, int8_t rssi);  /* after core handling    */
} NodeHooks;

void      wsnNodeSetup(uint8_t id, NodeRole role, const NodeHooks *hooks);
void      wsnNodeLoop(void);

NodePhase wsnNodePhase(void);
const char *wsnNodePhaseName(NodePhase p);
uint32_t  wsnNodeUptimeS(void);

/* Broadcast a demonstration command to the whole network.                    */
void      wsnNodeSendCommand(uint8_t target, uint8_t cmd, int32_t arg);
/* True when this node is pretending to be dead (`fail <self>`).              */
bool      wsnNodeIsSimulatedDown(void);
void      wsnNodeSetSimulatedDown(bool down);

/* Number of data frames this node has originated.                            */
uint32_t  wsnNodeReportsSent(void);

/* -----------------------------------------------------------------------------
 *  Network view.
 *
 *  Every node maintains one of these for every other node, built from the
 *  frames it overhears.  The sink's dashboard reads it directly, which is why
 *  it lives in the shared runtime rather than in the sink sketch.
 * -------------------------------------------------------------------------- */
typedef struct {
  bool        seen;
  uint32_t    lastHeardMs;
  uint8_t     role;
  uint8_t     hopsToSink;
  uint8_t     nextHop;
  int8_t      rssi;
  uint16_t    retx;
  uint8_t     alarm;
  uint16_t    schedVersion;
  bool        hasData;
  DataPayload last;          /* most recent reading from this node            */
  uint8_t     lastHops;
  char        lastPath[20];  /* e.g. "1->3->4"                                */
} NodeView;

const NodeView    *wsnNodeView(uint8_t id);
const LocPayload  *wsnNodeLastLocalization(void);   /* null until one arrives */
const AggPayload  *wsnNodeLastAggregate(void);      /* null until one arrives */

#endif /* WSN_NODE_H */
