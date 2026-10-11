/* =============================================================================
 *  NODE 3  --  CLUSTER HEAD / AGGREGATOR / ROUTER
 *                                   23CSE341 WSN Lab Evaluation 2
 * =============================================================================
 *
 *  ROLE      Cluster head: TDMA scheduler, time master, aggregator and router.
 *            Also a BLE anchor at a known position.
 *  SENSOR    MQ135  (air quality)
 *  WIRING    MQ135 AOUT -> GPIO 35  (ADC1_CH7, input-only)
 *            MQ135 VCC  -> 3V3      <-- NOT 5V; see Node 2's note and
 *                                       docs/WIRING.md
 *            MQ135 GND  -> GND
 *
 *  WHAT THIS NODE DOES
 *    SCHEDULING  Broadcasts the beacon in slot 0 carrying the slot map, the
 *                schedule version and the network time everyone synchronises
 *                to. Re-evaluates the schedule every superframe and updates it
 *                when a node raises an alarm, reports a backlog, goes silent
 *                or comes back -- each change bumping the version and carrying
 *                a plain-English reason.
 *    AGGREGATION Holds the DATA frames received from N1 and N2 instead of
 *                forwarding them, folds in its own MQ135 reading, and uplinks
 *                ONE aggregate per superframe carrying min/max/mean per field
 *                plus the OR of all alarm bits. Three packets in, one out --
 *                this is where the measured data reduction comes from.
 *    ROUTING     Forwards the aggregate to the sink over the lowest-cost route.
 *
 *  THIS IS THE NODE TO KILL for the route-failure demonstration: `fail 3`, or
 *  simply unplug it. N1 and N2 then detect the loss and re-route via N2 or
 *  directly to N4.
 * ========================================================================== */

#include "wsn_node.h"

void setup() {
  wsnNodeSetup(NODE_N3, ROLE_CLUSTER_HEAD, nullptr);
}

void loop() {
  wsnNodeLoop();
}
