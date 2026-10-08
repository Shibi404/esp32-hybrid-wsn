/* =============================================================================
 *  wsn_sensors.h  --  Sensor abstraction for the four node types.
 *
 *  One sensor per board:
 *      N1  DHT22      temperature + humidity   (digital, 1-wire)
 *      N2  MQ2        smoke / LPG              (analog, ADC1)
 *      N3  MQ135      air quality              (analog, ADC1)
 *      N4  HC-SR501   motion                   (digital, interrupt)
 *
 *  Which sensor is driven is chosen at *runtime* from NODE_ID, so this file
 *  compiles identically into all four sketches.
 *
 *  Three things this module takes seriously, because the lab guidelines ask
 *  for them and because they are what separates a working rig from a demo
 *  that falls over:
 *
 *  1. WARM-UP.  MQ sensors need their heater at temperature before the
 *     reading means anything.  Readings during the warm-up window are
 *     flagged DFLAG_WARMING rather than suppressed, so the network keeps
 *     running and the dashboard shows an honest "warming" state.
 *
 *  2. CALIBRATION.  R0 (sensor resistance in clean air) is measured once,
 *     stored in NVS and reused across power cycles.  Without it, ppm is
 *     meaningless.  `calib` re-runs it on demand.
 *
 *  3. FAULT HANDLING.  A failed read is not a zero.  Reads are retried, then
 *     the field's validity bit is cleared so that the aggregator excludes it
 *     from min/max/avg instead of averaging in a fake 0.
 * ========================================================================== */

#ifndef WSN_SENSORS_H
#define WSN_SENSORS_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef struct {
  float    temperature;    /* degC, NAN if invalid                            */
  float    humidity;       /* %RH,  NAN if invalid                            */
  float    gasPpm;         /* ppm,  NAN if invalid                            */
  float    gasRatio;       /* Rs/R0                                           */
  float    gasVolts;       /* conditioned sensor voltage (diagnostics)        */
  uint16_t gasRaw;         /* raw averaged ADC count (diagnostics)            */
  uint8_t  motion;         /* 0/1, 0xFF if this node has no PIR               */
  uint8_t  sensorMask;     /* SENS_* bits that are currently valid            */
  uint8_t  alarm;          /* ALARM_* bits currently asserted                 */
  bool     warming;        /* MQ heater still stabilising                     */
  uint32_t failCount;      /* cumulative failed reads (health indicator)      */
  uint32_t motionEvents;   /* cumulative PIR rising edges                     */
  uint32_t lastUpdateMs;
} SensorData;

void        sensorsBegin(void);
/* Non-blocking; call every loop.  Returns true when a fresh sample landed.   */
bool        sensorsUpdate(void);
const SensorData *sensorsGet(void);

/* Fill the on-air payload from the current reading.                          */
void        sensorsFillPayload(DataPayload *p);

bool        sensorsWarming(void);
uint32_t    sensorsWarmupRemainingMs(void);

/* Measure R0 in clean air and persist it to NVS.  Blocks for ~5 s.           */
void        sensorsCalibrate(void);
float       sensorsGetR0(void);
/* Human-readable dump for the `sensors` serial command.                      */
void        sensorsPrintStatus(void);
/* Name of the sensor this node carries, e.g. "MQ2 (smoke/LPG)".             */
const char *sensorsName(void);

#endif /* WSN_SENSORS_H */
