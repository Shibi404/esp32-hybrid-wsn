/* =============================================================================
 *  wsn_sensors.cpp
 * ========================================================================== */

#include "wsn_sensors.h"
#include "wsn_log.h"
#include <Preferences.h>
#include <DHT.h>
#include <math.h>

#define TAG "SENS"

static SensorData  s_d;
static Preferences s_nvs;

static DHT         s_dht(PIN_DHT22, DHT22);

static uint32_t    s_bootMs        = 0;
static uint32_t    s_lastSampleMs  = 0;
static uint32_t    s_lastDhtMs     = 0;
static uint8_t     s_dhtFails      = 0;
static uint8_t     s_gasFails      = 0;
static float       s_r0            = 0.0f;   /* clean-air resistance, kOhm    */
static bool        s_r0Valid       = false;
static bool        s_autoCalDone   = false;

/* PIR: the HC-SR501 already holds its output high for its configured dwell
 * time, so all we need is edge detection plus a short software debounce.     */
static volatile uint32_t s_motionEvents = 0;
static volatile uint32_t s_lastMotionUs = 0;

static void IRAM_ATTR pirIsr(void) {
  const uint32_t now = micros();
  if (now - s_lastMotionUs > 200000UL) {   /* 200 ms debounce                 */
    s_lastMotionUs = now;
    s_motionEvents++;
  }
}

/* -------------------------------------------------------------------------- */

static bool nodeHasDht(void)   { return NODE_ID == NODE_N1; }
static bool nodeHasMq2(void)   { return NODE_ID == NODE_N2; }
static bool nodeHasMq135(void) { return NODE_ID == NODE_N3; }
static bool nodeHasMq(void)    { return nodeHasMq2() || nodeHasMq135(); }
static bool nodeHasPir(void)   { return NODE_ID == NODE_N4; }

static int  mqPin(void)        { return nodeHasMq2() ? PIN_MQ2_AOUT : PIN_MQ135_AOUT; }
static float mqCleanRatio(void){ return nodeHasMq2() ? MQ2_CLEAN_AIR_RATIO
                                                     : MQ135_CLEAN_AIR_RATIO; }
static float mqCurveA(void)    { return nodeHasMq2() ? MQ2_SMOKE_A  : MQ135_CO2_A; }
static float mqCurveB(void)    { return nodeHasMq2() ? MQ2_SMOKE_B  : MQ135_CO2_B; }
static float mqAlarmPpm(void)  { return nodeHasMq2() ? ALARM_SMOKE_PPM
                                                     : ALARM_AIRQ_PPM; }

const char *sensorsName(void) {
  if (nodeHasDht())   return "DHT22 (temperature + humidity)";
  if (nodeHasMq2())   return "MQ2 (smoke / LPG)";
  if (nodeHasMq135()) return "MQ135 (air quality)";
  if (nodeHasPir())   return "HC-SR501 (motion)";
  return "none";
}

/* -----------------------------------------------------------------------------
 *  ADC front end.
 *
 *  The ESP32 SAR ADC is noisy and noticeably non-linear near the rails, so we
 *  multisample and use the Arduino core's calibrated millivolt reading
 *  (analogReadMilliVolts) which applies the factory eFuse characterisation.
 *  Both MQ modules run from 3V3 with no divider, so MQ_DIVIDER_GAIN is 1.0.
 * -------------------------------------------------------------------------- */
static bool mqReadVolts(float *volts, uint16_t *rawOut) {
  const int pin = mqPin();
  uint32_t mvAcc  = 0;
  uint32_t rawAcc = 0;

  for (int i = 0; i < MQ_ADC_SAMPLES; i++) {
    mvAcc  += analogReadMilliVolts(pin);
    rawAcc += analogRead(pin);
    delayMicroseconds(200);
  }

  const uint16_t raw = (uint16_t)(rawAcc / MQ_ADC_SAMPLES);
  const float    v   = (mvAcc / (float)MQ_ADC_SAMPLES) / 1000.0f * MQ_DIVIDER_GAIN;

  if (rawOut) *rawOut = raw;
  if (volts)  *volts  = v;

  /* A floating analog pin reads as noise near zero; a shorted or saturated
   * one pins at full scale.  Either way the reading is not a gas measurement,
   * so report the failure rather than publishing a number.                   */
  if (raw < 8)    { LOGW(TAG, "MQ ADC reads ~0 (raw=%u) - sensor disconnected?", raw); return false; }
  if (raw > 4080) { LOGW(TAG, "MQ ADC saturated (raw=%u) - check supply/wiring", raw); return false; }
  return true;
}

/* Rs = ((Vsupply / Vout) - 1) * RL                                           */
static bool mqReadRs(float *rs, float *volts, uint16_t *raw) {
  float v;
  if (!mqReadVolts(&v, raw)) return false;
  if (v < 0.01f) return false;

  const float r = ((MQ_SUPPLY_V / v) - 1.0f) * MQ_RL_KOHM;
  if (!(r > 0.0f) || isinf(r)) return false;

  if (rs)    *rs    = r;
  if (volts) *volts = v;
  return true;
}

/* -------------------------------------------------------------------------- */

float sensorsGetR0(void) { return s_r0; }

void sensorsCalibrate(void) {
  if (!nodeHasMq()) {
    LOGW(TAG, "calibrate: this node has no MQ sensor");
    return;
  }

  LOGI(TAG, "Calibrating R0 in clean air - keep the sensor away from gas sources");

  float   acc = 0.0f;
  uint8_t ok  = 0;
  for (uint8_t i = 0; i < 25; i++) {
    float rs;
    if (mqReadRs(&rs, nullptr, nullptr)) { acc += rs; ok++; }
    delay(200);
  }

  if (ok < 10) {
    LOGE(TAG, "Calibration FAILED - only %u/25 valid samples. Check wiring.", ok);
    return;
  }

  const float rsClean = acc / ok;
  s_r0      = rsClean / mqCleanRatio();
  s_r0Valid = true;

  s_nvs.begin(NVS_NAMESPACE, false);
  s_nvs.putFloat("r0", s_r0);
  s_nvs.end();

  LOGI(TAG, "Calibration OK: Rs(clean)=%.2f kOhm, clean-air ratio=%.2f, R0=%.2f kOhm "
            "(saved to NVS)", rsClean, mqCleanRatio(), s_r0);
  logCsv("CALIB,%s,%.3f,%.3f", sensorsName(), rsClean, s_r0);
}

/* -------------------------------------------------------------------------- */

void sensorsBegin(void) {
  memset(&s_d, 0, sizeof(s_d));
  s_d.temperature = NAN;
  s_d.humidity    = NAN;
  s_d.gasPpm      = NAN;
  s_d.gasRatio    = NAN;
  s_d.motion      = 0xFF;
  s_bootMs        = millis();

  if (nodeHasDht()) {
    s_dht.begin();
    LOGI(TAG, "DHT22 on GPIO%d (needs a 10k pull-up to 3V3)", PIN_DHT22);
  }

  if (nodeHasMq()) {
    const int pin = mqPin();
    analogSetPinAttenuation(pin, ADC_11db);   /* full 0..~3.1 V input span    */
    analogReadResolution(12);
    pinMode(pin, INPUT);

    s_nvs.begin(NVS_NAMESPACE, true);
    s_r0 = s_nvs.getFloat("r0", 0.0f);
    s_nvs.end();
    s_r0Valid = (s_r0 > 0.01f);

    LOGI(TAG, "%s on GPIO%d (ADC1), supply %.1fV, RL %.0fk, divider gain %.1f",
         sensorsName(), pin, MQ_SUPPLY_V, MQ_RL_KOHM, MQ_DIVIDER_GAIN);
    if (s_r0Valid) LOGI(TAG, "Loaded R0=%.2f kOhm from NVS", s_r0);
    else           LOGW(TAG, "No stored R0 - will auto-calibrate when warm-up ends");
  }

  if (nodeHasPir()) {
    pinMode(PIN_PIR, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR), pirIsr, RISING);
    s_d.motion = 0;
    LOGI(TAG, "HC-SR501 on GPIO%d (set the trigger jumper to H / repeatable)", PIN_PIR);
  }

  if (nodeHasMq() || nodeHasPir()) {
    LOGI(TAG, "Warm-up started: %lu s (readings are flagged WARMING until then)",
         (unsigned long)(SENSOR_WARMUP_MS / 1000));
  }
}

bool sensorsWarming(void) {
  if (!nodeHasMq() && !nodeHasPir()) return false;
  return (millis() - s_bootMs) < SENSOR_WARMUP_MS;
}

uint32_t sensorsWarmupRemainingMs(void) {
  const uint32_t el = millis() - s_bootMs;
  return (el >= SENSOR_WARMUP_MS) ? 0 : (SENSOR_WARMUP_MS - el);
}

/* -------------------------------------------------------------------------- */

static void updateDht(void) {
  /* The DHT22 cannot be polled faster than once every ~2 s; asking more often
   * returns the cached value or fails outright.                              */
  if (millis() - s_lastDhtMs < DHT_MIN_INTERVAL_MS) return;
  s_lastDhtMs = millis();

  const float h = s_dht.readHumidity();
  const float t = s_dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    s_dhtFails++;
    s_d.failCount++;
    if (s_dhtFails >= SENSOR_MAX_FAILS) {
      /* Stop publishing a stale value as if it were live.  Clearing the
       * validity bits makes the aggregator skip this field entirely.         */
      s_d.sensorMask &= (uint8_t)~(SENS_TEMP | SENS_HUM);
      s_d.temperature = NAN;
      s_d.humidity    = NAN;
      LOGE(TAG, "DHT22 failed %u consecutive reads - marking temp/hum invalid "
                "(check the 10k pull-up and the 3V3 supply)", s_dhtFails);
    } else {
      LOGW(TAG, "DHT22 read failed (%u/%u) - keeping last good value",
           s_dhtFails, SENSOR_MAX_FAILS);
    }
    return;
  }

  /* Range check: the DHT22 spec is -40..80 C and 0..100 %RH.  Anything else
   * is a framing error the library did not catch.                            */
  if (t < -40.0f || t > 80.0f || h < 0.0f || h > 100.0f) {
    s_dhtFails++;
    s_d.failCount++;
    LOGW(TAG, "DHT22 out of range (t=%.1f h=%.1f) - discarded", t, h);
    return;
  }

  if (s_dhtFails) LOGI(TAG, "DHT22 recovered after %u failed reads", s_dhtFails);
  s_dhtFails      = 0;
  s_d.temperature = t;
  s_d.humidity    = h;
  s_d.sensorMask |= (SENS_TEMP | SENS_HUM);

  if (t >= ALARM_TEMP_C) s_d.alarm |=  ALARM_TEMP;
  else                   s_d.alarm &= (uint8_t)~ALARM_TEMP;
}

static void updateMq(void) {
  float rs, volts;
  uint16_t raw;

  if (!mqReadRs(&rs, &volts, &raw)) {
    s_gasFails++;
    s_d.failCount++;
    if (s_gasFails >= SENSOR_MAX_FAILS) {
      s_d.sensorMask &= (uint8_t)~SENS_GAS;
      s_d.gasPpm   = NAN;
      s_d.gasRatio = NAN;
      LOGE(TAG, "MQ sensor failed %u consecutive reads - marking gas invalid",
           s_gasFails);
    }
    return;
  }

  if (s_gasFails) LOGI(TAG, "MQ sensor recovered after %u failed reads", s_gasFails);
  s_gasFails  = 0;
  s_d.gasRaw   = raw;
  s_d.gasVolts = volts;

  /* Auto-calibrate once, at the end of warm-up, if no R0 was ever stored.
   * Doing it at the end rather than at boot matters: calibrating a cold
   * heater bakes a wrong baseline into NVS.                                  */
  if (!s_r0Valid && !sensorsWarming() && !s_autoCalDone) {
    s_autoCalDone = true;
    LOGI(TAG, "Warm-up complete and no stored R0 - auto-calibrating now");
    sensorsCalibrate();
  }

  if (!s_r0Valid) {
    /* Without R0 we can still report the raw resistance, but not ppm.  Say so
     * rather than inventing a number.                                        */
    s_d.gasRatio = NAN;
    s_d.gasPpm   = NAN;
    s_d.sensorMask &= (uint8_t)~SENS_GAS;
    return;
  }

  const float ratio = rs / s_r0;
  float ppm = mqCurveA() * powf(ratio, mqCurveB());

  /* Clamp to the datasheet's usable detection span.  Extrapolating the log-log
   * fit outside it produces absurd values that would skew every average.     */
  if (!isfinite(ppm)) ppm = NAN;
  else if (ppm < 1.0f)     ppm = 1.0f;
  else if (ppm > 10000.0f) ppm = 10000.0f;

  s_d.gasRatio = ratio;
  s_d.gasPpm   = ppm;
  s_d.sensorMask |= SENS_GAS;

  if (isfinite(ppm) && ppm >= mqAlarmPpm()) {
    if (!(s_d.alarm & ALARM_GAS))
      LOGW(TAG, "GAS ALARM: %.0f ppm >= %.0f ppm threshold", ppm, mqAlarmPpm());
    s_d.alarm |= ALARM_GAS;
  } else {
    if (s_d.alarm & ALARM_GAS) LOGI(TAG, "Gas alarm cleared (%.0f ppm)", ppm);
    s_d.alarm &= (uint8_t)~ALARM_GAS;
  }
}

static void updatePir(void) {
  const uint32_t ev = s_motionEvents;
  const bool level  = digitalRead(PIN_PIR) == HIGH;

  if (ev != s_d.motionEvents) {
    s_d.motionEvents = ev;
    LOGI(TAG, "PIR motion event #%lu", (unsigned long)ev);
  }

  s_d.motion      = level ? 1 : 0;
  s_d.sensorMask |= SENS_MOTION;

  if (level) s_d.alarm |=  ALARM_MOTION;
  else       s_d.alarm &= (uint8_t)~ALARM_MOTION;
}

bool sensorsUpdate(void) {
  if (millis() - s_lastSampleMs < SENSOR_SAMPLE_MS) return false;
  s_lastSampleMs = millis();

  if (nodeHasDht()) updateDht();
  if (nodeHasMq())  updateMq();
  if (nodeHasPir()) updatePir();

  s_d.warming      = sensorsWarming();
  s_d.lastUpdateMs = millis();
  return true;
}

const SensorData *sensorsGet(void) { return &s_d; }

void sensorsFillPayload(DataPayload *p) {
  memset(p, 0, sizeof(*p));
  p->temperature = (s_d.sensorMask & SENS_TEMP) ? s_d.temperature : NAN;
  p->humidity    = (s_d.sensorMask & SENS_HUM)  ? s_d.humidity    : NAN;
  p->gasPpm      = (s_d.sensorMask & SENS_GAS)  ? s_d.gasPpm      : NAN;
  p->gasRatio    = (s_d.sensorMask & SENS_GAS)  ? s_d.gasRatio    : NAN;
  p->motion      = (s_d.sensorMask & SENS_MOTION) ? s_d.motion    : 0xFF;
  p->sensorMask  = s_d.sensorMask;
  p->alarm       = s_d.alarm;
  p->flags       = s_d.warming ? DFLAG_WARMING : 0;
}

void sensorsPrintStatus(void) {
  Serial.println();
  Serial.printf("  Sensor        : %s\r\n", sensorsName());
  Serial.printf("  Warm-up       : %s", sensorsWarming() ? "IN PROGRESS" : "complete");
  if (sensorsWarming())
    Serial.printf(" (%lu s remaining)", (unsigned long)(sensorsWarmupRemainingMs() / 1000));
  Serial.println();

  if (nodeHasDht()) {
    Serial.printf("  Temperature   : %s\r\n",
                  (s_d.sensorMask & SENS_TEMP) ? String(s_d.temperature, 2).c_str()
                                               : "INVALID");
    Serial.printf("  Humidity      : %s\r\n",
                  (s_d.sensorMask & SENS_HUM)  ? String(s_d.humidity, 2).c_str()
                                               : "INVALID");
  }
  if (nodeHasMq()) {
    Serial.printf("  ADC raw       : %u\r\n", s_d.gasRaw);
    Serial.printf("  Sensor voltage: %.3f V\r\n", s_d.gasVolts);
    Serial.printf("  R0 (clean air): %s\r\n",
                  s_r0Valid ? String(s_r0, 2).c_str() : "NOT CALIBRATED - run `calib`");
    Serial.printf("  Rs/R0 ratio   : %s\r\n",
                  isfinite(s_d.gasRatio) ? String(s_d.gasRatio, 3).c_str() : "n/a");
    Serial.printf("  Concentration : %s ppm\r\n",
                  isfinite(s_d.gasPpm) ? String(s_d.gasPpm, 0).c_str() : "n/a");
  }
  if (nodeHasPir()) {
    Serial.printf("  Motion        : %s\r\n", s_d.motion ? "DETECTED" : "none");
    Serial.printf("  Motion events : %lu\r\n", (unsigned long)s_d.motionEvents);
  }
  Serial.printf("  Valid fields  : %s%s%s%s\r\n",
                (s_d.sensorMask & SENS_TEMP)   ? "temp " : "",
                (s_d.sensorMask & SENS_HUM)    ? "hum "  : "",
                (s_d.sensorMask & SENS_GAS)    ? "gas "  : "",
                (s_d.sensorMask & SENS_MOTION) ? "motion": "");
  Serial.printf("  Alarms        : %s%s%s\r\n",
                (s_d.alarm & ALARM_GAS)    ? "GAS "    : "",
                (s_d.alarm & ALARM_MOTION) ? "MOTION " : "",
                (s_d.alarm & ALARM_TEMP)   ? "TEMP "   : "");
  Serial.printf("  Failed reads  : %lu\r\n", (unsigned long)s_d.failCount);
  Serial.println();
}
