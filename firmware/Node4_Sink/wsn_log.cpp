/* =============================================================================
 *  wsn_log.cpp
 * ========================================================================== */

#include "wsn_log.h"
#include <WiFi.h>
#include <stdarg.h>

static LogLevel s_level   = LVL_INFO;
static bool     s_csv     = true;

void logBegin(uint32_t baud) {
  Serial.begin(baud);
  /* Give the USB-serial bridge a moment so the boot banner is not cut off on
   * boards that enumerate slowly.  Bounded, so a headless node never stalls. */
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) { delay(10); }
  delay(100);
}

void logSetLevel(LogLevel lvl) { s_level = lvl; }
LogLevel logGetLevel(void)     { return s_level; }
void logSetCsv(bool on)        { s_csv = on; }
bool logCsvEnabled(void)       { return s_csv; }

static char levelChar(LogLevel l) {
  switch (l) {
    case LVL_ERROR: return 'E';
    case LVL_WARN:  return 'W';
    case LVL_INFO:  return 'I';
    default:        return 'D';
  }
}

void logPrintf(LogLevel lvl, const char *tag, const char *fmt, ...) {
  if (lvl > s_level) return;

  char body[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  const uint32_t ms = millis();
  Serial.printf("[%6lu.%03lu][%s][%-5s][%c] %s\r\n",
                (unsigned long)(ms / 1000), (unsigned long)(ms % 1000),
                NODE_NAME[NODE_ID <= NUM_NODES ? NODE_ID : 0],
                tag, levelChar(lvl), body);
}

void logCsv(const char *fmt, ...) {
  if (!s_csv) return;

  char body[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  Serial.printf("#CSV,%s,%lu,%s\r\n",
                NODE_NAME[NODE_ID <= NUM_NODES ? NODE_ID : 0],
                (unsigned long)millis(), body);
}

const char *logMacStr(const uint8_t *mac, char *buf) {
  snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

void logSection(const char *title) {
  const size_t width = 72;
  const size_t tlen  = strlen(title);
  const size_t pad   = (tlen + 2 >= width) ? 0 : (width - tlen - 2) / 2;

  Serial.println();
  for (size_t i = 0; i < width; i++) Serial.print('=');
  Serial.println();
  for (size_t i = 0; i < pad; i++) Serial.print(' ');
  Serial.print(' '); Serial.print(title); Serial.println();
  for (size_t i = 0; i < width; i++) Serial.print('=');
  Serial.println();
}

void logBanner(void) {
  uint8_t mac[6];
  char    macbuf[18];
  WiFi.macAddress(mac);

  Serial.println();
  logSection("ESP32 HYBRID WSN  --  23CSE341 LAB EVALUATION 2");
  Serial.printf("  Node        : %s (id %u)\r\n",
                NODE_NAME[NODE_ID <= NUM_NODES ? NODE_ID : 0], NODE_ID);
  Serial.printf("  Role        : %s\r\n", wsnRoleName(NODE_ROLE));
  Serial.printf("  STA MAC     : %s\r\n", logMacStr(mac, macbuf));
  Serial.printf("  Firmware    : %s\r\n", WSN_FW_VERSION);
  Serial.printf("  Arduino core: %d.%d.%d\r\n",
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("  Channel     : %d      Superframe: %lu ms / %d slots\r\n",
                WSN_WIFI_CHANNEL, (unsigned long)TDMA_SUPERFRAME_MS, TDMA_NUM_SLOTS);
  Serial.printf("  Phy CCA     : %s     Light sleep: %s\r\n",
                ENABLE_PHY_CCA ? "on " : "off",
                ENABLE_LIGHT_SLEEP ? "on" : "off (radio-off sleep)");

  /* Warn loudly if the MAC table was never filled in -- this is the number one
   * reason a freshly cloned build appears to do nothing.                     */
  if (wsnNodeIdFromMac(mac) == 0) {
    Serial.println();
    Serial.println("  *** WARNING: this board's MAC is NOT in the NODE_MAC table.  ***");
    Serial.println("  *** Paste it into common/wsn_config.cpp and re-flash all 4.  ***");
  } else if (wsnNodeIdFromMac(mac) != NODE_ID) {
    Serial.println();
    Serial.printf("  *** WARNING: NODE_MAC says this board is N%u, but this sketch "
                  "is N%u. ***\r\n", wsnNodeIdFromMac(mac), NODE_ID);
  }
  Serial.println();

  /* CSV header rows, so the logger knows the column layout of each stream.   */
  logCsv("HEADER,kind,...");
}
