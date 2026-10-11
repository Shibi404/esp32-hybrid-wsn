/* =============================================================================
 *  NODE 2  --  SENSOR / RELAY NODE  23CSE341 WSN Lab Evaluation 2
 * =============================================================================
 *
 *  ROLE      Sensor node AND relay; a BLE anchor at a known position.
 *  SENSOR    MQ2  (smoke / LPG / combustible gas)
 *  WIRING    MQ2 AOUT -> GPIO 34  (ADC1_CH6, input-only)
 *            MQ2 VCC  -> 3V3      <-- NOT 5V.  On 3V3 the analog output can
 *                                     never exceed the GPIO's 3.3 V limit, so
 *                                     no divider is fitted.  See docs/WIRING.md
 *                                     for what this costs in accuracy.
 *            MQ2 GND  -> GND
 *
 *  WHAT THIS NODE DOES
 *    BLE    advertises as "WSN-N2" and serves as a localization anchor; its
 *           coordinates in wsn_config.cpp must match where you actually put it.
 *    Wi-Fi  originates a sensor report in TDMA slot 2, and FORWARDS traffic for
 *           N1 whenever N1's route switches to the alternate path N1->N2->N4.
 *           That relay role is what makes the route-recovery demonstration
 *           show real multi-hop forwarding rather than just a direct fallback.
 *    Gas alarm here is one of the two events that drive a dynamic schedule
 *    update: crossing the smoke threshold earns this node the extra slot.
 *
 *  MQ2 NOTES
 *    - 120 s warm-up at each power-up; readings are flagged WARMING until then.
 *    - R0 is auto-calibrated in clean air at the end of warm-up and saved to
 *      NVS. Re-run it any time with the `calib` command.
 *    - Without a 24-48 h burn-in the absolute ppm figure is indicative only;
 *      relative response is what the demonstration relies on.
 * ========================================================================== */

#include "wsn_node.h"

void setup() {
  wsnNodeSetup(NODE_N2, ROLE_SENSOR_RELAY, nullptr);
}

void loop() {
  wsnNodeLoop();
}
