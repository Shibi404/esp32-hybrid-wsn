/* =============================================================================
 *  wsn_tdma.h  --  Dynamic schedule-based (TDMA) communication.
 *
 *  Superframe: 8 slots x 250 ms = 2000 ms, repeating.
 *
 *    slot 0  BEACON   N3 broadcasts the schedule and the time reference
 *    slot 1  DATA     N1 transmits
 *    slot 2  DATA     N2 transmits
 *    slot 3  DYNAMIC  unassigned by default -> a sleep slot.  The cluster head
 *                     grants it on demand (alarm or backlog), which is what
 *                     makes the schedule genuinely dynamic.
 *    slot 4  AGG      N3 uplinks the aggregate to N4
 *    slot 5  CAP      contention access period -- joins, route repair, and the
 *                     collision demonstration
 *    slot 6  STATUS   N4 broadcasts the network performance summary
 *    slot 7  SLEEP    everybody idle
 *
 *  TIME SYNC.  The cluster head is the time master.  Its beacon carries both
 *  the superframe origin and its own clock, so every node derives a shared
 *  `network time`.  That shared clock is what makes slot boundaries agree
 *  across nodes, and it is also what lets the sink compute a true end-to-end
 *  delay from a timestamp written by a different board.
 *
 *  SLEEP.  A node is awake for the beacon slot, for any slot it owns, and for
 *  slots in which it might have to receive (a relay must hear its children, an
 *  aggregator must hear its sources).  Everything else is a sleep slot: the
 *  radio is powered down with esp_wifi_stop(), which is a real ~40 mA saving
 *  and not a simulation.  The sink never sleeps -- it is mains-powered and
 *  must keep the dashboard reachable, which is the normal design choice for a
 *  WSN gateway.
 * ========================================================================== */

#ifndef WSN_TDMA_H
#define WSN_TDMA_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef struct {
  uint8_t  owner[TDMA_NUM_SLOTS];    /* node id, 0 = unassigned (sleep)       */
  uint8_t  owner2[TDMA_NUM_SLOTS];   /* second owner -- collision demo only   */
  uint8_t  kind[TDMA_NUM_SLOTS];     /* SlotKind                              */
  uint16_t version;
  uint8_t  reason;                   /* SchedReason for the last change       */
  uint8_t  reasonNode;
} Schedule;

typedef struct {
  uint32_t superframes;
  uint32_t slotsOwned;        /* slots in which this node was allowed to TX   */
  uint32_t slotsUsed;         /* of those, slots in which it actually did     */
  uint32_t slotsUnused;       /* owned but nothing to send -> released        */
  uint32_t sleepSlots;
  uint32_t sleepMs;           /* cumulative radio-off time                    */
  uint32_t scheduleUpdates;
  uint32_t beaconsRx;
  uint32_t beaconsMissed;
  int32_t  lastSyncOffsetMs;  /* correction applied by the last beacon        */
} TdmaStats;

void     tdmaBegin(void);
void     tdmaLoop(void);

/* --- time ----------------------------------------------------------------- */
uint32_t tdmaNetworkTimeMs(void);          /* shared clock, ms               */
bool     tdmaIsSynced(void);
void     tdmaApplyBeacon(const BeaconPayload *b, uint32_t rxLocalMs);

/* --- slots ---------------------------------------------------------------- */
uint8_t  tdmaCurrentSlot(void);
uint32_t tdmaSlotElapsedMs(void);
uint32_t tdmaSlotRemainingMs(void);
uint32_t tdmaSuperframeIndex(void);
/* True only inside this node's own slot, and outside the guard bands.        */
bool     tdmaIsMySlot(void);
bool     tdmaIsSlotMine(uint8_t slot);
bool     tdmaIsContentionSlot(void);
uint8_t  tdmaSlotOwner(uint8_t slot);
uint8_t  tdmaSlotKind(uint8_t slot);

/* --- cluster-head side ---------------------------------------------------- */
void     tdmaBuildBeacon(BeaconPayload *b);
/* Fold a node's alarm/backlog report into the next scheduling decision.      */
void     tdmaNoteNodeReport(uint8_t nodeId, uint8_t alarm, uint8_t queueLen);
/* Note that a node transmitted in its slot (resets its silence counter).     */
void     tdmaNoteNodeActive(uint8_t nodeId);
/* Called by a node sketch when it actually transmits in its own slot.  A slot
 * that ends without this having been called is counted as UNUSED and reported
 * to the cluster head, which is how slot reclamation is triggered.           */
void     tdmaMarkSlotUsed(void);
/* Re-evaluate the schedule; returns true if it changed.                      */
bool     tdmaReviewSchedule(void);
/* Put two owners in one slot for the contention demonstration.               */
void     tdmaScheduleCollision(uint8_t a, uint8_t b);

/* --- sleep ---------------------------------------------------------------- */
bool     tdmaShouldSleepNow(void);
void     tdmaEnterSleep(uint32_t ms);
float    tdmaDutyCyclePct(void);

const Schedule  *tdmaSchedule(void);
const TdmaStats *tdmaStats(void);
void     tdmaPrintSchedule(void);
void     tdmaPrintStats(void);
const char *tdmaSlotKindName(uint8_t k);

#endif /* WSN_TDMA_H */
