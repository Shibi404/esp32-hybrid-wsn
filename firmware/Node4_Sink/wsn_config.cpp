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
  { 0xB0, 0xCB, 0xD8, 0x0C, 0xDB, 0xA8 },   /* [1] N1  Sensor      (DHT22)    */
  { 0xB0, 0xCB, 0xD8, 0x0A, 0x7A, 0xB8 },   /* [2] N2  Sensor/Relay(MQ2)      */
  { 0xB0, 0xCB, 0xD8, 0x0A, 0x93, 0x88 },   /* [3] N3  Cluster head(MQ135)    */
  { 0xB0, 0xCB, 0xD8, 0x0A, 0x7A, 0x38 }    /* [4] N4  Sink        (PIR)      */
};

/*  ---- LABEL YOUR BOARDS ----------------------------------------------------
 *  Write the node number on each board with tape before you flash anything.
 *  Two of these differ only in the last byte:
 *
 *      N2  B0:CB:D8:0A:7A:B8        <-- ...7A:B8
 *      N4  B0:CB:D8:0A:7A:38        <-- ...7A:38
 *
 *  Mixing those two up puts the sink's firmware on a sensor node, and the
 *  symptom is a network that half-works in a confusing way.  The boot banner
 *  does catch it -- it compares this table against the board's own MAC and
 *  prints a warning -- so read the banner on every board after flashing.
 * ------------------------------------------------------------------------- */

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

/* -----------------------------------------------------------------------------
 *  >>> STEP 3 (optional): INTENDED TOPOLOGY <<<
 *
 *  1 = the two nodes may use each other as a next hop; 0 = they may not.
 *  Row/column 0 is unused.  The matrix is symmetric.
 *
 *  The single 0 pair, N1<->N4, is what forces N1's traffic through a relay
 *  and so makes the multi-hop, aggregation and route-recovery behaviour real
 *  on a bench where every board can hear every other.  If you space the nodes
 *  out so that N1 genuinely cannot reach the sink, set this to all 1s and the
 *  cost metric will discover the same topology by itself.
 *
 *            N1  N2  N3  N4                                                  */
const uint8_t ROUTE_LINK_ALLOWED[NUM_NODES + 1][NUM_NODES + 1] = {
  { 0,  0,  0,  0,  0 },   /* [0] unused                                     */
  { 0,  0,  1,  1,  0 },   /* N1: may use N2 or N3   -- NOT the sink directly */
  { 0,  1,  0,  1,  1 },   /* N2: may use N1, N3 or the sink                 */
  { 0,  1,  1,  0,  1 },   /* N3: may use N1, N2 or the sink                 */
  { 0,  0,  1,  1,  0 }    /* N4: may use N2 or N3                           */
};

bool routeLinkAllowed(uint8_t a, uint8_t b) {
  if (a < 1 || a > NUM_NODES || b < 1 || b > NUM_NODES) return false;
  return ROUTE_LINK_ALLOWED[a][b] != 0;
}

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
