/* =============================================================================
 *  wsn_config.cpp  --  Per-deployment constants.
 *
 *  >>> EDIT THE TWO TABLES BELOW BEFORE FLASHING. <<<
 * ========================================================================== */

#include "wsn_config.h"

/* -----------------------------------------------------------------------------
 *  >>> STEP 1: BOARD MAC ADDRESSES <<<
 *
 *  Flash tools/print_mac/print_mac.ino to each board in turn, note the printed
 *  "STA MAC", and paste the six bytes here.  Index 0 is a placeholder so that
 *  NODE_MAC[1] is node 1.
 *
 *  Getting these wrong is the single most common cause of "nothing is
 *  communicating" -- ESP-NOW silently discards frames to an unregistered peer.
 * -------------------------------------------------------------------------- */
const uint8_t NODE_MAC[NUM_NODES + 1][6] = {
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   /* [0] unused                     */
  { 0x24, 0x6F, 0x28, 0x00, 0x00, 0x01 },   /* [1] N1  Sensor      (DHT22)    */
  { 0x24, 0x6F, 0x28, 0x00, 0x00, 0x02 },   /* [2] N2  Sensor/Relay(MQ2)      */
  { 0x24, 0x6F, 0x28, 0x00, 0x00, 0x03 },   /* [3] N3  Cluster head(MQ135)    */
  { 0x24, 0x6F, 0x28, 0x00, 0x00, 0x04 }    /* [4] N4  Sink        (PIR)      */
};

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* -----------------------------------------------------------------------------
 *  >>> STEP 2: NODE POSITIONS (metres) <<<
 *
 *  Lay out a local coordinate frame on the bench or floor with a tape measure.
 *  The defaults describe a 4.0 m x 3.0 m rectangle:
 *
 *        y
 *        ^
 *    3.0 + N4(0,3)
 *        |
 *        |            N1(2.0,1.5)  <- mobile node, ground truth
 *        |
 *    0.0 + N2(0,0) ------------- N3(4,0) --> x
 *        0.0                     4.0
 *
 *  N2/N3/N4 are anchors: their coordinates are *given* to the algorithm.
 *  N1's entry is ground truth only -- it is never fed into the solver, it is
 *  used after the fact to compute the localization error.  Update it at
 *  runtime with the `settrue <x> <y>` serial command when you move N1.
 * -------------------------------------------------------------------------- */
Point2 NODE_POS[NUM_NODES + 1] = {
  { 0.0f, 0.0f },     /* [0] unused                                          */
  { 2.0f, 1.5f },     /* [1] N1  MOBILE -- ground truth for the error metric */
  { 0.0f, 0.0f },     /* [2] N2  anchor                                      */
  { 4.0f, 0.0f },     /* [3] N3  anchor                                      */
  { 0.0f, 3.0f }      /* [4] N4  anchor                                      */
};

bool NODE_IS_ANCHOR[NUM_NODES + 1] = { false, false, true, true, true };

const char *NODE_NAME[NUM_NODES + 1] = { "??", "N1", "N2", "N3", "N4" };

/* -------------------------------------------------------------------------- */

uint8_t wsnNodeIdFromMac(const uint8_t *mac) {
  if (!mac) return 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (memcmp(mac, NODE_MAC[i], 6) == 0) return i;
  }
  return 0;
}

const uint8_t *wsnMacForNode(uint8_t id) {
  if (id == BROADCAST_ID) return BROADCAST_MAC;
  if (id < 1 || id > NUM_NODES) return nullptr;
  return NODE_MAC[id];
}

const char *wsnRoleName(NodeRole r) {
  switch (r) {
    case ROLE_SENSOR:       return "SENSOR";
    case ROLE_SENSOR_RELAY: return "SENSOR/RELAY";
    case ROLE_CLUSTER_HEAD: return "CLUSTER-HEAD";
    case ROLE_SINK:         return "SINK/GATEWAY";
    default:                return "UNKNOWN";
  }
}
