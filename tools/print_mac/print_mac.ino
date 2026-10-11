/* =============================================================================
 *  print_mac.ino  --  Read each board's Wi-Fi MAC address.
 *
 *  Flash this to each of the four ESP32s in turn, open the Serial Monitor at
 *  115200 baud, and copy the printed line straight into the NODE_MAC table in
 *  common/wsn_config.cpp.
 *
 *  ESP-NOW addresses peers by MAC, and it silently discards frames sent to an
 *  unregistered peer -- no error, no warning, just a network that does
 *  nothing. Getting this table right is the single most important setup step.
 *
 *  NOTE: use the STA MAC. The SoftAP MAC is a different address (normally the
 *  STA MAC with the second-least-significant bit of the first byte set), and
 *  using it here would break ESP-NOW even though it looks almost identical.
 * ========================================================================== */

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(600);

  WiFi.mode(WIFI_STA);
  delay(100);

  uint8_t mac[6];
  WiFi.macAddress(mac);

  Serial.println();
  Serial.println("================================================================");
  Serial.println("  ESP32 MAC ADDRESS  --  for common/wsn_config.cpp");
  Serial.println("================================================================");
  Serial.printf("\r\n  STA MAC : %02X:%02X:%02X:%02X:%02X:%02X\r\n\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println("  Paste this line into the NODE_MAC table, on the row for");
  Serial.println("  whichever node this board will be:");
  Serial.printf("\r\n    { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X },\r\n\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println("================================================================");
}

void loop() {
  delay(5000);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("  { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X },\r\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
