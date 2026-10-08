/* =============================================================================
 *  wsn_aggregate.cpp
 * ========================================================================== */

#include "wsn_aggregate.h"
#include "wsn_log.h"
#include "wsn_metrics.h"
#include <math.h>

#define TAG "AGG"

/* One running accumulator per field.  Keeping count per field (rather than one
 * count for the whole sample) is what lets a node with a dead humidity sensor
 * still contribute its temperature.                                          */
typedef struct {
  float    mn, mx, sum;
  uint16_t n;
} FieldAcc;

static FieldAcc s_t, s_h, s_g;
static uint8_t  s_srcMask;
static uint8_t  s_alarmOr;
static uint8_t  s_sensorMask;
static uint8_t  s_samples;
static uint8_t  s_motionCount;
static uint16_t s_rawPackets;
static uint16_t s_rawBytes;
static uint32_t s_oldestTxMs;

static void accReset(FieldAcc *a) { a->mn = NAN; a->mx = NAN; a->sum = 0.0f; a->n = 0; }

static void accAdd(FieldAcc *a, float v) {
  if (!isfinite(v)) return;
  if (a->n == 0) { a->mn = v; a->mx = v; }
  else { if (v < a->mn) a->mn = v; if (v > a->mx) a->mx = v; }
  a->sum += v;
  a->n++;
}

static float accAvg(const FieldAcc *a) { return a->n ? (a->sum / a->n) : NAN; }

void aggBegin(void) {
  accReset(&s_t); accReset(&s_h); accReset(&s_g);
  s_srcMask = s_alarmOr = s_sensorMask = s_samples = s_motionCount = 0;
  s_rawPackets = s_rawBytes = 0;
  s_oldestTxMs = 0;
}

static void addCommon(uint8_t srcId, const DataPayload *d, uint32_t txTimeMs) {
  if (srcId >= 1 && srcId <= NUM_NODES) s_srcMask |= (uint8_t)(1u << (srcId - 1));

  /* Only fold in a field the source itself marked valid.                     */
  if (d->sensorMask & SENS_TEMP)   accAdd(&s_t, d->temperature);
  if (d->sensorMask & SENS_HUM)    accAdd(&s_h, d->humidity);
  if (d->sensorMask & SENS_GAS)    accAdd(&s_g, d->gasPpm);
  if ((d->sensorMask & SENS_MOTION) && d->motion == 1) s_motionCount++;

  s_sensorMask |= d->sensorMask;
  s_alarmOr    |= d->alarm;          /* aggregation must never swallow an alarm */
  s_samples++;

  /* Track the earliest contributor so the sink can measure worst-case delay
   * for everything inside the aggregate, not just the last reading added.   */
  if (txTimeMs && (s_oldestTxMs == 0 || txTimeMs < s_oldestTxMs)) s_oldestTxMs = txTimeMs;
}

void aggAddSample(uint8_t srcId, const DataPayload *d, uint32_t txTimeMs,
                  size_t onAirBytes) {
  addCommon(srcId, d, txTimeMs);
  s_rawPackets++;
  s_rawBytes += (uint16_t)onAirBytes;

  LOGD(TAG, "Folded in N%u (sample %u): t=%.1f h=%.1f gas=%.0f alarm=0x%02X",
       srcId, s_samples, d->temperature, d->humidity, d->gasPpm, d->alarm);
}

void aggAddLocal(const DataPayload *d, uint32_t txTimeMs) {
  addCommon(NODE_ID, d, txTimeMs);
  /* The local reading would also have cost a packet had it been sent on its
   * own, so it counts toward the "raw" baseline the reduction is measured
   * against.                                                                 */
  s_rawPackets++;
  s_rawBytes += (uint16_t)(WSN_HDR_LEN + sizeof(DataPayload) + 2);
}

bool    aggHasData(void)      { return s_samples > 0; }
uint8_t aggSampleCount(void)  { return s_samples; }

uint8_t aggSourceCount(void) {
  uint8_t c = 0, m = s_srcMask;
  while (m) { c += (m & 1); m >>= 1; }
  return c;
}

bool aggBuild(AggPayload *out) {
  if (!s_samples) return false;

  memset(out, 0, sizeof(*out));
  out->srcMask     = s_srcMask;
  out->nSamples    = s_samples;
  out->alarmOr     = s_alarmOr;
  out->sensorMask  = s_sensorMask;
  out->tMin = s_t.mn; out->tMax = s_t.mx; out->tAvg = accAvg(&s_t);
  out->hMin = s_h.mn; out->hMax = s_h.mx; out->hAvg = accAvg(&s_h);
  out->gMin = s_g.mn; out->gMax = s_g.mx; out->gAvg = accAvg(&s_g);
  out->motionCount = s_motionCount;
  out->rawPackets  = s_rawPackets;
  out->rawBytes    = s_rawBytes;
  out->aggBytes    = (uint16_t)(WSN_HDR_LEN + sizeof(AggPayload) + 2);
  out->oldestTxMs  = s_oldestTxMs;

  const float red = s_rawBytes
      ? 100.0f * (1.0f - (float)out->aggBytes / (float)s_rawBytes) : 0.0f;

  LOGI(TAG, "Aggregated %u readings from %u source(s) [mask 0x%02X]: "
            "%u packets / %u B  ->  1 packet / %u B  (%.1f %% fewer bytes)",
       s_samples, aggSourceCount(), s_srcMask,
       s_rawPackets, s_rawBytes, out->aggBytes, red);
  LOGI(TAG, "  temp %.1f/%.1f/%.1f  hum %.1f/%.1f/%.1f  gas %.0f/%.0f/%.0f "
            "(min/avg/max)  alarms 0x%02X",
       out->tMin, out->tAvg, out->tMax,
       out->hMin, out->hAvg, out->hMax,
       out->gMin, out->gAvg, out->gMax, out->alarmOr);

  metricsNoteAggregation(s_rawPackets, s_rawBytes, out->aggBytes);

  aggBegin();        /* start the next window */
  return true;
}

void aggPrint(void) {
  Serial.println();
  Serial.println("  --- AGGREGATION WINDOW -------------------------------------");
  Serial.printf("   Samples held  : %u from %u source(s) (mask 0x%02X)\r\n",
                s_samples, aggSourceCount(), s_srcMask);
  if (s_samples) {
    Serial.printf("   Temperature   : min %.1f  avg %.1f  max %.1f  (n=%u)\r\n",
                  s_t.mn, accAvg(&s_t), s_t.mx, s_t.n);
    Serial.printf("   Humidity      : min %.1f  avg %.1f  max %.1f  (n=%u)\r\n",
                  s_h.mn, accAvg(&s_h), s_h.mx, s_h.n);
    Serial.printf("   Gas (ppm)     : min %.0f  avg %.0f  max %.0f  (n=%u)\r\n",
                  s_g.mn, accAvg(&s_g), s_g.mx, s_g.n);
    Serial.printf("   Motion reports: %u    alarms 0x%02X\r\n", s_motionCount, s_alarmOr);
    Serial.printf("   Raw so far    : %u packets, %u B\r\n", s_rawPackets, s_rawBytes);
  }
  const AggMetrics *m = metricsAgg();
  Serial.printf("   Cumulative    : %lu raw packets (%lu B) -> %lu aggregates "
                "(%lu B)\r\n",
                (unsigned long)m->rawPackets, (unsigned long)m->rawBytes,
                (unsigned long)m->aggPackets, (unsigned long)m->aggBytes);
  Serial.printf("   DATA REDUCTION: %.2f %% of bytes, %.2f %% of packets\r\n",
                m->byteReductionPct, m->packetReductionPct);
  Serial.println();
}
