/* =============================================================================
 *  wsn_metrics.h  --  Performance measurement.
 *
 *  All seven metrics named in the lab guidelines are measured (four are
 *  required).  Where each one is computed, and from what:
 *
 *    Packet Delivery Ratio   sink    received / expected, per originator,
 *                                    derived from sequence numbers
 *    Packet loss             sink    expected - received
 *    End-to-end delay        sink    sinkNetworkTime - frame.txTimeMs, using
 *                                    the TDMA beacon's shared clock
 *    Throughput              sink    delivered payload bits / elapsed seconds
 *    Retransmissions         all     MAC retry counter, carried to the sink in
 *                                    the data payload and summed there
 *    Localization error      N1      ||estimated - ground truth||, with RMSE
 *    Data reduction %        N3      1 - aggregatedBytes / rawBytes
 *
 *  A note on `expected`: it is derived from the sequence-number span actually
 *  observed (lastSeq - firstSeq + 1) rather than from a count of what the
 *  source believes it sent.  That is the honest definition at the receiver --
 *  it counts gaps in the sequence, so a packet lost in transit is visible even
 *  though the sink never heard about it.
 * ========================================================================== */

#ifndef WSN_METRICS_H
#define WSN_METRICS_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef struct {
  uint32_t rxPackets;
  uint32_t rxBytes;          /* on-air bytes, header included                 */
  uint32_t rxPayloadBytes;   /* application payload only                      */
  uint32_t duplicates;
  uint32_t outOfOrder;
  uint16_t firstSeq, lastSeq;
  bool     seen;
  uint32_t expected;
  uint32_t lost;
  uint64_t delaySumMs;
  uint32_t delayMin, delayMax;
  uint32_t delayCount;
  uint32_t firstRxMs, lastRxMs;
  uint32_t retxReported;     /* cumulative retransmissions at the source      */
  uint8_t  lastHops;
  char     lastPath[20];
} SrcMetrics;

typedef struct {
  uint32_t rawPackets;       /* packets folded into aggregates                */
  uint32_t rawBytes;         /* what those packets would have cost on the air */
  uint32_t aggPackets;       /* aggregates actually sent                      */
  uint32_t aggBytes;
  float    byteReductionPct;
  float    packetReductionPct;
} AggMetrics;

void metricsBegin(void);

/* Sink side: record one delivered data/aggregate frame.                      */
void metricsNoteRx(const WsnHeader *h, size_t frameLen, size_t payloadLen,
                   uint16_t srcRetx, uint32_t nowNetMs);
/* Aggregator side: record one aggregation event.                             */
void metricsNoteAggregation(uint16_t rawPackets, uint16_t rawBytes,
                            uint16_t aggBytes);

const SrcMetrics *metricsForSource(uint8_t id);
const AggMetrics *metricsAgg(void);

float    metricsPdr(uint8_t id);          /* 0..1, per source                 */
float    metricsPdrOverall(void);
uint32_t metricsLostOverall(void);
uint32_t metricsRxOverall(void);
uint32_t metricsAvgDelayMs(void);
uint32_t metricsMaxDelayMs(void);
float    metricsThroughputBps(void);      /* application payload bits/second  */
uint32_t metricsRetxOverall(void);
float    metricsDataReductionPct(void);

void metricsReset(void);
void metricsPrint(void);
/* One CSV row summarising the current state -- emitted once per superframe.  */
void metricsEmitCsv(void);

#endif /* WSN_METRICS_H */
