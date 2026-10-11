/* =============================================================================
 *  wsn_metrics.cpp
 * ========================================================================== */

#include "wsn_metrics.h"
#include "wsn_log.h"

#define TAG "METRIC"

static SrcMetrics s_src[NUM_NODES + 1];
static AggMetrics s_agg;
static uint32_t   s_startMs = 0;

void metricsBegin(void) {
  memset(s_src, 0, sizeof(s_src));
  memset(&s_agg, 0, sizeof(s_agg));
  for (uint8_t i = 0; i <= NUM_NODES; i++) s_src[i].delayMin = 0xFFFFFFFF;
  s_startMs = millis();
}

void metricsReset(void) {
  metricsBegin();
  LOGI(TAG, "Performance counters reset");
}

/* -------------------------------------------------------------------------- */

void metricsNoteRx(const WsnHeader *h, size_t frameLen, size_t payloadLen,
                   uint16_t srcRetx, uint32_t nowNetMs) {
  const uint8_t id = h->src;
  if (id < 1 || id > NUM_NODES) return;

  SrcMetrics &m = s_src[id];
  const uint32_t now = millis();

  if (!m.seen) {
    m.seen      = true;
    m.firstSeq  = h->seq;
    m.lastSeq   = h->seq;
    m.firstRxMs = now;
  } else {
    /* Duplicates would otherwise inflate PDR above 1.0 and understate delay. */
    if (h->seq == m.lastSeq) { m.duplicates++; return; }
    if ((int16_t)(h->seq - m.lastSeq) < 0) { m.outOfOrder++; }
    else                                     { m.lastSeq = h->seq; }
  }

  m.rxPackets++;
  m.rxBytes        += frameLen;
  m.rxPayloadBytes += payloadLen;
  m.lastRxMs        = now;
  m.retxReported    = srcRetx;
  m.lastHops        = h->hop;
  wsnPathToString(h, m.lastPath, sizeof(m.lastPath));

  /* Sequence-span accounting: gaps are losses, visible at the receiver.      */
  m.expected = (uint32_t)(m.lastSeq - m.firstSeq) + 1;
  m.lost     = (m.expected > m.rxPackets) ? (m.expected - m.rxPackets) : 0;

  /* End-to-end delay, using the beacon-synchronised network clock.  A
   * negative value means the clocks have not converged yet (it happens for
   * the first frame or two after a resync); it is discarded rather than
   * folded in as a bogus zero.                                              */
  if (h->txTimeMs && nowNetMs >= h->txTimeMs) {
    const uint32_t d = nowNetMs - h->txTimeMs;
    if (d < 60000UL) {                       /* sanity bound                  */
      m.delaySumMs += d;
      m.delayCount++;
      if (d < m.delayMin) m.delayMin = d;
      if (d > m.delayMax) m.delayMax = d;
    }
  }

  logCsv("RX,%u,%u,%u,%u,%lu,%s", id, h->seq, h->hop, (unsigned)frameLen,
         (unsigned long)((h->txTimeMs && nowNetMs >= h->txTimeMs)
                         ? (nowNetMs - h->txTimeMs) : 0),
         m.lastPath);
}

void metricsNoteAggregation(uint16_t rawPackets, uint16_t rawBytes,
                            uint16_t aggBytes) {
  s_agg.rawPackets += rawPackets;
  s_agg.rawBytes   += rawBytes;
  s_agg.aggPackets += 1;
  s_agg.aggBytes   += aggBytes;

  s_agg.byteReductionPct = s_agg.rawBytes
      ? 100.0f * (1.0f - (float)s_agg.aggBytes / (float)s_agg.rawBytes) : 0.0f;
  s_agg.packetReductionPct = s_agg.rawPackets
      ? 100.0f * (1.0f - (float)s_agg.aggPackets / (float)s_agg.rawPackets) : 0.0f;

  LOGI(TAG, "AGGREGATION: %u packets (%u B) -> 1 packet (%u B)  |  "
            "cumulative reduction: %.1f %% of bytes, %.1f %% of packets",
       rawPackets, rawBytes, aggBytes,
       s_agg.byteReductionPct, s_agg.packetReductionPct);
  logCsv("AGG,%u,%u,%u,%.2f,%.2f", rawPackets, rawBytes, aggBytes,
         s_agg.byteReductionPct, s_agg.packetReductionPct);
}

/* -------------------------------------------------------------------------- */

const SrcMetrics *metricsForSource(uint8_t id) {
  return (id <= NUM_NODES) ? &s_src[id] : &s_src[0];
}
const AggMetrics *metricsAgg(void) { return &s_agg; }

float metricsPdr(uint8_t id) {
  if (id > NUM_NODES) return 0.0f;
  const SrcMetrics &m = s_src[id];
  if (!m.expected) return 0.0f;
  const float pdr = (float)m.rxPackets / (float)m.expected;
  return pdr > 1.0f ? 1.0f : pdr;
}

float metricsPdrOverall(void) {
  uint32_t rx = 0, exp = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) { rx += s_src[i].rxPackets; exp += s_src[i].expected; }
  if (!exp) return 0.0f;
  const float p = (float)rx / (float)exp;
  return p > 1.0f ? 1.0f : p;
}

uint32_t metricsRxOverall(void) {
  uint32_t rx = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) rx += s_src[i].rxPackets;
  return rx;
}

uint32_t metricsLostOverall(void) {
  uint32_t lost = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) lost += s_src[i].lost;
  return lost;
}

uint32_t metricsAvgDelayMs(void) {
  uint64_t sum = 0; uint32_t n = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) { sum += s_src[i].delaySumMs; n += s_src[i].delayCount; }
  return n ? (uint32_t)(sum / n) : 0;
}

uint32_t metricsMaxDelayMs(void) {
  uint32_t mx = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) if (s_src[i].delayMax > mx) mx = s_src[i].delayMax;
  return mx;
}

float metricsThroughputBps(void) {
  const uint32_t el = millis() - s_startMs;
  if (el < 1000) return 0.0f;
  uint32_t bytes = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) bytes += s_src[i].rxPayloadBytes;
  return (float)bytes * 8.0f * 1000.0f / (float)el;
}

uint32_t metricsRetxOverall(void) {
  uint32_t r = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) r += s_src[i].retxReported;
  return r;
}

float metricsDataReductionPct(void) { return s_agg.byteReductionPct; }

/* -------------------------------------------------------------------------- */

void metricsEmitCsv(void) {
  logCsv("PERF,%.4f,%lu,%lu,%lu,%.1f,%lu,%.2f",
         metricsPdrOverall(), (unsigned long)metricsRxOverall(),
         (unsigned long)metricsLostOverall(), (unsigned long)metricsAvgDelayMs(),
         metricsThroughputBps(), (unsigned long)metricsRetxOverall(),
         metricsDataReductionPct());
}

void metricsPrint(void) {
  const uint32_t el = (millis() - s_startMs) / 1000;

  Serial.println();
  Serial.println("  --- PERFORMANCE ANALYSIS -----------------------------------");
  Serial.printf("   Measurement window : %lu s\r\n", (unsigned long)el);
  Serial.println();
  Serial.println("   Per source:");
  Serial.println("    src   rx     exp    lost   PDR%    avgDelay  maxDelay  hops  path");
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    const SrcMetrics &m = s_src[i];
    if (!m.seen) continue;
    const uint32_t avg = m.delayCount ? (uint32_t)(m.delaySumMs / m.delayCount) : 0;
    Serial.printf("    N%-4u %-6lu %-6lu %-6lu %-7.2f %-9lu %-9lu %-5u %s\r\n",
                  i, (unsigned long)m.rxPackets, (unsigned long)m.expected,
                  (unsigned long)m.lost, metricsPdr(i) * 100.0f,
                  (unsigned long)avg, (unsigned long)m.delayMax,
                  m.lastHops, m.lastPath);
  }

  Serial.println();
  Serial.println("   Network totals:");
  Serial.printf("    1. Packet Delivery Ratio : %.2f %%   (%lu received / %lu expected)\r\n",
                metricsPdrOverall() * 100.0f,
                (unsigned long)metricsRxOverall(),
                (unsigned long)(metricsRxOverall() + metricsLostOverall()));
  Serial.printf("    2. Packet loss           : %lu packets  (%.2f %%)\r\n",
                (unsigned long)metricsLostOverall(),
                (1.0f - metricsPdrOverall()) * 100.0f);
  Serial.printf("    3. End-to-end delay      : %lu ms mean, %lu ms max\r\n",
                (unsigned long)metricsAvgDelayMs(),
                (unsigned long)metricsMaxDelayMs());
  Serial.printf("    4. Throughput            : %.1f bit/s  (application payload)\r\n",
                metricsThroughputBps());
  Serial.printf("    5. Retransmissions       : %lu (reported by sources)\r\n",
                (unsigned long)metricsRetxOverall());
  Serial.printf("    7. Data reduction        : %.2f %% of bytes, %.2f %% of packets\r\n",
                s_agg.byteReductionPct, s_agg.packetReductionPct);
  Serial.printf("       (aggregated %lu raw packets / %lu B into %lu packets / %lu B)\r\n",
                (unsigned long)s_agg.rawPackets, (unsigned long)s_agg.rawBytes,
                (unsigned long)s_agg.aggPackets, (unsigned long)s_agg.aggBytes);
  Serial.println("    6. Localization error    : see the `loc` command on N1");
  Serial.printf("   Duplicates discarded      : ");
  for (uint8_t i = 1; i <= NUM_NODES; i++)
    if (s_src[i].seen) Serial.printf("N%u=%lu ", i, (unsigned long)s_src[i].duplicates);
  Serial.println();
  Serial.println();
}
