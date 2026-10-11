/* =============================================================================
 *  wsn_log.h  --  Two output streams multiplexed onto one serial port.
 *
 *  HUMAN stream   [  12.345][N1][MAC ] CCA busy, backoff frozen at 5
 *                 Readable during the live demonstration and in screenshots.
 *
 *  MACHINE stream #CSV,mac,12345,1,3,...
 *                 Prefixed with "#CSV," so tools/serial_logger.py can pick the
 *                 rows out of the same console without a second UART.
 *
 *  Keeping both on one port matters: during the demo you want to *see* the
 *  protocol working, and afterwards you want the same run's numbers in a
 *  spreadsheet without re-running anything.
 * ========================================================================== */

#ifndef WSN_LOG_H
#define WSN_LOG_H

#include <Arduino.h>
#include "wsn_config.h"

typedef enum : uint8_t { LVL_ERROR = 0, LVL_WARN = 1, LVL_INFO = 2, LVL_DEBUG = 3 } LogLevel;

void logBegin(uint32_t baud);
void logSetLevel(LogLevel lvl);
LogLevel logGetLevel(void);

/* Enable/disable the "#CSV," stream at runtime (`csv on` / `csv off`).       */
void logSetCsv(bool on);
bool logCsvEnabled(void);

void logPrintf(LogLevel lvl, const char *tag, const char *fmt, ...);
void logCsv(const char *fmt, ...);

/* A banner printed once at boot -- role, MAC, version, build settings.       */
void logBanner(void);
/* "AA:BB:CC:DD:EE:FF" into a caller-supplied 18-byte buffer.                 */
const char *logMacStr(const uint8_t *mac, char *buf);

#define LOGE(tag, ...)  logPrintf(LVL_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...)  logPrintf(LVL_WARN,  tag, __VA_ARGS__)
#define LOGI(tag, ...)  logPrintf(LVL_INFO,  tag, __VA_ARGS__)
#define LOGD(tag, ...)  logPrintf(LVL_DEBUG, tag, __VA_ARGS__)

/* A horizontal rule with a centred title -- used to separate demo phases so
 * screenshots are self-explanatory.                                         */
void logSection(const char *title);

#endif /* WSN_LOG_H */
