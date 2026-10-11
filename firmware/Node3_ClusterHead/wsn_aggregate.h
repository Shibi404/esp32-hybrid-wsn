/* =============================================================================
 *  wsn_aggregate.h  --  Data aggregation at the cluster head (N3).
 *
 *  Within one superframe the cluster head collects every DATA frame it
 *  receives from N1 and N2, folds in its own reading, and emits a SINGLE
 *  aggregate carrying min / max / mean for each field instead of forwarding
 *  three packets individually.
 *
 *  Why min/max/mean rather than just the mean: a mean alone hides the event
 *  the network exists to detect.  If one node reports 900 ppm of smoke and two
 *  report 100 ppm, the mean (367) looks unremarkable while the max (900) is
 *  the alarm.  The alarm bits are OR-ed across contributors for the same
 *  reason -- aggregation must not be able to swallow an alarm.
 *
 *  Fields that a contributor marked invalid are excluded from its statistics
 *  rather than counted as zero, so a dead sensor cannot drag an average down.
 * ========================================================================== */

#ifndef WSN_AGGREGATE_H
#define WSN_AGGREGATE_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

void aggBegin(void);
/* Fold one received DATA frame into the current window.                      */
void aggAddSample(uint8_t srcId, const DataPayload *d, uint32_t txTimeMs,
                  size_t onAirBytes);
/* Fold this node's own reading in.                                           */
void aggAddLocal(const DataPayload *d, uint32_t txTimeMs);

bool    aggHasData(void);
uint8_t aggSampleCount(void);
uint8_t aggSourceCount(void);

/* Build the aggregate and clear the window.  Returns false if nothing was
 * collected, which is the normal case when every source was asleep.          */
bool aggBuild(AggPayload *out);

void aggPrint(void);

#endif /* WSN_AGGREGATE_H */
