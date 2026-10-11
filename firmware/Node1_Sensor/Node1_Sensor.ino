/* =============================================================================
 *  NODE 1  --  SENSOR NODE          23CSE341 WSN Lab Evaluation 2
 * =============================================================================
 *
 *  ROLE      Sensor node, and the MOBILE node for BLE localization.
 *  SENSOR    DHT22  (temperature + humidity)
 *  WIRING    DHT22 DATA -> GPIO 4, with a 10k pull-up to 3V3
 *            DHT22 VCC  -> 3V3,  GND -> GND
 *
 *  WHAT THIS NODE DOES
 *    BLE    advertises as "WSN-N1", scans for the three anchors (N2, N3, N4),
 *           filters their RSSI, converts to distance, trilaterates its own
 *           position and scores the localization error against the tape-
 *           measured ground truth in wsn_config.cpp.
 *    Wi-Fi  originates one sensor report per superframe in TDMA slot 1, using
 *           the CSMA/CA MAC (carrier sense, backoff, ACK, retransmission), and
 *           routes it to the sink via the lowest-cost next hop -- normally N3,
 *           and N2 or a direct hop when N3 fails.
 *    Sleeps through every slot it neither owns nor needs to listen in.
 *
 *  BEFORE FLASHING
 *    1. Paste all four board MAC addresses into common/wsn_config.cpp
 *    2. Set the anchor coordinates and this node's true position there too
 *    3. Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
 * ========================================================================== */

#include "wsn_node.h"

void setup() {
  wsnNodeSetup(NODE_N1, ROLE_SENSOR, nullptr);
}

void loop() {
  wsnNodeLoop();
}
