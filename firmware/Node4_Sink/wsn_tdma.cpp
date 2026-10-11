/* =============================================================================
 *  wsn_tdma.cpp  --  Superframe timing, dynamic slot allocation, sleep/wake.
 * ========================================================================== */

#include "wsn_tdma.h"
#include "wsn_log.h"
#include "wsn_mac.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>

#define TAG "TDMA"

static Schedule  s_sched;
static TdmaStats s_stats;

static int32_t   s_timeOffsetMs = 0;    /* network time = millis() + offset   */
static bool      s_synced       = false;
static uint32_t  s_lastBeaconMs = 0;
static uint8_t   s_prevSlot     = 0xFF;
static uint32_t  s_slotEnterMs  = 0;
static bool      s_slotUsed     = false;
static bool      s_radioOff     = false;

/* Cluster-head bookkeeping for dynamic scheduling.                           */
static uint8_t   s_alarmOf[NUM_NODES + 1]   = { 0 };
static uint8_t   s_backlogOf[NUM_NODES + 1] = { 0 };
static uint8_t   s_silentFor[NUM_NODES + 1] = { 0 };
static uint8_t   s_dynGrantedTo = 0;
static uint8_t   s_revokedOwner[TDMA_NUM_SLOTS] = { 0 };

const char *tdmaSlotKindName(uint8_t k) {
  switch (k) {
    case SK_BEACON: return "BEACON";
    case SK_DATA:   return "DATA";
    case SK_AGG:    return "AGG";
    case SK_CAP:    return "CAP";
    case SK_STATUS: return "STATUS";
    case SK_SLEEP:  return "SLEEP";
    default:        return "?";
  }
}

/* ========================================================================== */
/*  Default schedule                                                          */
/* ========================================================================== */

static void buildDefaultSchedule(void) {
  memset(&s_sched, 0, sizeof(s_sched));

  s_sched.owner[SLOT_BEACON]      = CLUSTER_HEAD_ID; s_sched.kind[SLOT_BEACON]      = SK_BEACON;
  s_sched.owner[SLOT_N1_DATA]     = NODE_N1;         s_sched.kind[SLOT_N1_DATA]     = SK_DATA;
  s_sched.owner[SLOT_N2_DATA]     = NODE_N2;         s_sched.kind[SLOT_N2_DATA]     = SK_DATA;
  s_sched.owner[SLOT_DYNAMIC]     = 0;               s_sched.kind[SLOT_DYNAMIC]     = SK_SLEEP;
  s_sched.owner[SLOT_AGG_UPLINK]  = CLUSTER_HEAD_ID; s_sched.kind[SLOT_AGG_UPLINK]  = SK_AGG;
  s_sched.owner[SLOT_CAP]         = BROADCAST_ID;    s_sched.kind[SLOT_CAP]         = SK_CAP;
  s_sched.owner[SLOT_SINK_STATUS] = SINK_ID;         s_sched.kind[SLOT_SINK_STATUS] = SK_STATUS;
  s_sched.owner[SLOT_SLEEP]       = 0;               s_sched.kind[SLOT_SLEEP]       = SK_SLEEP;

  s_sched.version = 1;
  s_sched.reason  = SCHED_INIT;
}

void tdmaBegin(void) {
  buildDefaultSchedule();
  memset(&s_stats, 0, sizeof(s_stats));

  /* The cluster head defines the time base, so it is synced by definition.
   * Everyone else waits for a beacon before trusting slot boundaries.        */
  if (NODE_ID == CLUSTER_HEAD_ID) {
    s_synced       = true;
    s_timeOffsetMs = 0;
  }

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  LOGI(TAG, "Superframe %lu ms = %d slots x %lu ms (guard %lu ms)",
       (unsigned long)TDMA_SUPERFRAME_MS, TDMA_NUM_SLOTS,
       (unsigned long)TDMA_SLOT_MS, (unsigned long)TDMA_GUARD_MS);
  tdmaPrintSchedule();
}

/* ========================================================================== */
/*  Time                                                                      */
/* ========================================================================== */

uint32_t tdmaNetworkTimeMs(void) { return (uint32_t)((int32_t)millis() + s_timeOffsetMs); }
bool     tdmaIsSynced(void)      { return s_synced; }

void tdmaApplyBeacon(const BeaconPayload *b, uint32_t rxLocalMs) {
  if (NODE_ID == CLUSTER_HEAD_ID) return;   /* the master never follows       */

  /* Align the local clock to the beacon's.  The one-way propagation delay is
   * well under a millisecond over a few metres, so it is ignored; the
   * dominant error is the few ms the frame spent in the receive queue, which
   * is why the beacon is handled promptly in the receive path.               */
  const int32_t newOffset = (int32_t)b->netTimeMs - (int32_t)rxLocalMs;
  const int32_t delta     = newOffset - s_timeOffsetMs;

  s_timeOffsetMs          = newOffset;
  s_stats.lastSyncOffsetMs = delta;
  s_lastBeaconMs          = millis();
  s_stats.beaconsRx++;

  if (!s_synced) {
    s_synced = true;
    LOGI(TAG, "Time synchronised to N%u (offset %+ld ms) - slot boundaries are "
              "now network-wide", b->chId, (long)s_timeOffsetMs);
  } else if (abs((int)delta) > 25) {
    LOGW(TAG, "Clock corrected by %+ld ms (drift)", (long)delta);
  }

  /* Adopt a newer schedule.  The version number is what makes this safe: a
   * stale beacon overheard late can never roll the network backwards.        */
  if (b->schedVersion != s_sched.version) {
    const uint16_t old = s_sched.version;
    memcpy(s_sched.owner,  b->slotOwner,  TDMA_NUM_SLOTS);
    memcpy(s_sched.owner2, b->slotOwner2, TDMA_NUM_SLOTS);
    memcpy(s_sched.kind,   b->slotKind,   TDMA_NUM_SLOTS);
    s_sched.version    = b->schedVersion;
    s_sched.reason     = b->reason;
    s_sched.reasonNode = b->reasonNode;
    s_stats.scheduleUpdates++;

    char who[12] = "";
    if (b->reasonNode) snprintf(who, sizeof(who), " (N%u)", b->reasonNode);
    LOGI(TAG, "SCHEDULE UPDATE v%u -> v%u: %s%s",
         old, s_sched.version, wsnSchedReasonName(b->reason), who);
    tdmaPrintSchedule();
    logCsv("SCHED,%u,%u,%u", s_sched.version, b->reason, b->reasonNode);
  }
}

/* ========================================================================== */
/*  Slot arithmetic                                                           */
/* ========================================================================== */

uint32_t tdmaSuperframeIndex(void) { return tdmaNetworkTimeMs() / TDMA_SUPERFRAME_MS; }

uint8_t tdmaCurrentSlot(void) {
  return (uint8_t)((tdmaNetworkTimeMs() % TDMA_SUPERFRAME_MS) / TDMA_SLOT_MS);
}

uint32_t tdmaSlotElapsedMs(void) {
  return (tdmaNetworkTimeMs() % TDMA_SUPERFRAME_MS) % TDMA_SLOT_MS;
}

uint32_t tdmaSlotRemainingMs(void) { return TDMA_SLOT_MS - tdmaSlotElapsedMs(); }

uint8_t tdmaSlotOwner(uint8_t slot) {
  return (slot < TDMA_NUM_SLOTS) ? s_sched.owner[slot] : 0;
}

uint8_t tdmaSlotKind(uint8_t slot) {
  return (slot < TDMA_NUM_SLOTS) ? s_sched.kind[slot] : (uint8_t)SK_SLEEP;
}

bool tdmaIsSlotMine(uint8_t slot) {
  if (slot >= TDMA_NUM_SLOTS) return false;
  return s_sched.owner[slot] == NODE_ID || s_sched.owner2[slot] == NODE_ID;
}

bool tdmaIsMySlot(void) {
  if (!s_synced) return false;
  const uint8_t slot = tdmaCurrentSlot();
  if (!tdmaIsSlotMine(slot)) return false;

  /* Guard bands at both edges absorb the few ms of clock skew between boards,
   * so a node never transmits into its neighbour's slot.                     */
  const uint32_t el = tdmaSlotElapsedMs();
  return el >= TDMA_GUARD_MS && el <= (TDMA_SLOT_MS - TDMA_GUARD_MS);
}

bool tdmaIsContentionSlot(void) {
  const uint8_t slot = tdmaCurrentSlot();
  return s_sched.kind[slot] == SK_CAP;
}

/* ========================================================================== */
/*  Sleep / wake                                                              */
/* ========================================================================== */

/* A node must stay awake for:
 *   - the beacon slot (schedule and time sync),
 *   - any slot it owns,
 *   - the contention slot (route repair, joins),
 *   - slots in which it may have to RECEIVE: the aggregator must hear its
 *     sources, a relay must hear the node it relays for, the sink must hear
 *     the uplink.
 * Everything else is a sleep slot.                                           */
static bool slotNeedsRadio(uint8_t slot) {
  if (s_sched.kind[slot] == SK_BEACON) return true;
  if (tdmaIsSlotMine(slot))            return true;
  if (s_sched.kind[slot] == SK_CAP)    return true;

  switch (NODE_ROLE) {
    case ROLE_SINK:
      /* The gateway is mains-powered and hosts the dashboard; it never sleeps.
       * This is the conventional WSN asymmetry -- energy is only scarce at the
       * leaves.                                                              */
      return true;

    case ROLE_CLUSTER_HEAD:
      /* Must hear every sensor slot in order to aggregate them.              */
      return s_sched.kind[slot] == SK_DATA || s_sched.kind[slot] == SK_STATUS;

    case ROLE_SENSOR_RELAY:
      /* Must hear the node it may be relaying for, plus the status broadcast. */
      return s_sched.kind[slot] == SK_DATA || s_sched.kind[slot] == SK_STATUS;

    case ROLE_SENSOR:
    default:
      /* A leaf only needs the status broadcast beyond its own slot.          */
      return s_sched.kind[slot] == SK_STATUS;
  }
}

bool tdmaShouldSleepNow(void) {
  if (!s_synced) return false;
  return !slotNeedsRadio(tdmaCurrentSlot());
}

void tdmaEnterSleep(uint32_t ms) {
  if (ms < 40) return;                     /* not worth the transition cost   */
  if (NODE_ROLE == ROLE_SINK) return;      /* the gateway stays up            */

  digitalWrite(PIN_LED, LOW);
  s_stats.sleepSlots++;
  s_stats.sleepMs += ms;

  LOGD(TAG, "Sleeping %lu ms (slot %u is not mine and I need not listen)",
       (unsigned long)ms, tdmaCurrentSlot());

#if ENABLE_LIGHT_SLEEP
  /* Whole-SoC light sleep.  esp_timer is RTC-backed, so millis() keeps
   * advancing across the sleep and the TDMA clock survives it.  Serial output
   * stops for the duration, which is why this is off by default for a demo. */
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_light_sleep_start();
#else
  /* Radio-off sleep: power down the Wi-Fi MAC and PHY only.  This is a real
   * ~40 mA saving on an ESP32, while the CPU stays alive so the serial log
   * keeps narrating the demonstration.                                      */
  if (!s_radioOff) {
    esp_wifi_stop();
    s_radioOff = true;
  }
  delay(ms);
#endif
}

/* Bring the radio back before a slot in which we transmit or receive.        */
static void wakeRadio(void) {
#if !ENABLE_LIGHT_SLEEP
  if (!s_radioOff) return;
  esp_wifi_start();
  esp_wifi_set_channel(WSN_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  s_radioOff = false;
  /* esp_wifi_stop() can tear down the ESP-NOW peer list; macSend() detects the
   * resulting ESP_ERR_ESPNOW_NOT_INIT and re-registers, but doing a cheap
   * explicit nudge here avoids losing the first frame after every wake.      */
  delay(5);
#endif
}

float tdmaDutyCyclePct(void) {
  const uint32_t total = s_stats.superframes * TDMA_SUPERFRAME_MS;
  if (!total) return 100.0f;
  const uint32_t awake = (total > s_stats.sleepMs) ? (total - s_stats.sleepMs) : 0;
  return 100.0f * (float)awake / (float)total;
}

/* ========================================================================== */
/*  Per-slot bookkeeping                                                      */
/* ========================================================================== */

void tdmaLoop(void) {
  if (!s_synced) {
    /* Not yet synced: a non-head node simply waits, listening, until the first
     * beacon lands.  It must not transmit on a guessed slot boundary.        */
    return;
  }

  const uint8_t slot = tdmaCurrentSlot();
  if (slot == s_prevSlot) return;

  /* ---- a slot boundary was crossed --------------------------------------- */
  if (s_prevSlot != 0xFF && tdmaIsSlotMine(s_prevSlot)) {
    s_stats.slotsOwned++;
    if (s_slotUsed) {
      s_stats.slotsUsed++;
    } else {
      /* UNUSED SLOT HANDLING: the node owned the slot but had nothing to
       * send, so the slot went idle.  It is reported to the cluster head via
       * the next HELLO, which is how the head learns it can reclaim it.      */
      s_stats.slotsUnused++;
      LOGI(TAG, "Slot %u was mine but UNUSED (nothing queued) - released, the "
                "cluster head may reclaim it", s_prevSlot);
      logCsv("SLOTUNUSED,%u", s_prevSlot);
    }
  }

  if (slot == 0 && s_prevSlot != 0xFF) {
    s_stats.superframes++;

    /* A non-head node that has not heard a beacon for two superframes has
     * lost its time master.                                                 */
    if (NODE_ID != CLUSTER_HEAD_ID &&
        (millis() - s_lastBeaconMs) > (2 * TDMA_SUPERFRAME_MS)) {
      s_stats.beaconsMissed++;
      if (s_stats.beaconsMissed == 2) {
        LOGW(TAG, "No beacon for %lu ms - the cluster head may be down. "
                  "Staying awake and falling back to contention access.",
             (unsigned long)(millis() - s_lastBeaconMs));
      }
    } else {
      s_stats.beaconsMissed = 0;
    }
  }

  s_prevSlot    = slot;
  s_slotEnterMs = millis();
  s_slotUsed    = false;

  const bool mine = tdmaIsSlotMine(slot);
  digitalWrite(PIN_LED, mine ? HIGH : LOW);

  if (slotNeedsRadio(slot)) wakeRadio();

  LOGD(TAG, "--- slot %u (%s, owner N%u)%s ---", slot,
       tdmaSlotKindName(s_sched.kind[slot]), s_sched.owner[slot],
       mine ? "  <= MINE" : "");
}

/* Called by the node sketches when they actually transmit in their slot.     */
void tdmaMarkSlotUsed(void) { s_slotUsed = true; }

/* ========================================================================== */
/*  Cluster head: dynamic schedule maintenance                                */
/* ========================================================================== */

void tdmaNoteNodeReport(uint8_t nodeId, uint8_t alarm, uint8_t queueLen) {
  if (nodeId < 1 || nodeId > NUM_NODES) return;
  s_alarmOf[nodeId]   = alarm;
  s_backlogOf[nodeId] = queueLen;
  s_silentFor[nodeId] = 0;
}

void tdmaNoteNodeActive(uint8_t nodeId) {
  if (nodeId >= 1 && nodeId <= NUM_NODES) s_silentFor[nodeId] = 0;
}

static void bumpSchedule(uint8_t reason, uint8_t node) {
  s_sched.version++;
  s_sched.reason     = reason;
  s_sched.reasonNode = node;
  s_stats.scheduleUpdates++;
  char who[12] = "";
  if (node) snprintf(who, sizeof(who), " (N%u)", node);
  LOGW(TAG, "SCHEDULE UPDATED to v%u: %s%s",
       s_sched.version, wsnSchedReasonName(reason), who);
  tdmaPrintSchedule();
  logCsv("SCHED,%u,%u,%u", s_sched.version, reason, node);
}

/* Re-evaluate the schedule once per superframe.  Four things can change it:
 *
 *   1. a node raises an alarm   -> grant it the dynamic slot (more frequent
 *                                  reporting while the event is happening)
 *   2. the alarm clears         -> return the slot to sleep (save energy)
 *   3. a node goes silent       -> revoke its slot (stop wasting airtime)
 *   4. a silent node comes back -> restore it
 *
 * This is the answer to "why and how was the schedule updated": it tracks
 * demand, and demand is driven by sensed events and by liveness.             */
bool tdmaReviewSchedule(void) {
  if (NODE_ID != CLUSTER_HEAD_ID) return false;
  bool changed = false;

  /* --- 1/2: dynamic slot follows the alarm state -------------------------- */
  uint8_t wants = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == SINK_ID) continue;
    if (s_alarmOf[i]) { wants = i; break; }
  }
  if (!wants) {
    for (uint8_t i = 1; i <= NUM_NODES; i++) {
      if (i == SINK_ID) continue;
      if (s_backlogOf[i] >= SCHED_BACKLOG_LIMIT) { wants = i; break; }
    }
  }

  if (wants && s_dynGrantedTo != wants) {
    s_sched.owner[SLOT_DYNAMIC] = wants;
    s_sched.kind[SLOT_DYNAMIC]  = SK_DATA;
    s_dynGrantedTo = wants;
    bumpSchedule(s_alarmOf[wants] ? SCHED_ALARM_GRANT : SCHED_BACKLOG_GRANT, wants);
    changed = true;
  } else if (!wants && s_dynGrantedTo) {
    const uint8_t was = s_dynGrantedTo;
    s_sched.owner[SLOT_DYNAMIC] = 0;
    s_sched.kind[SLOT_DYNAMIC]  = SK_SLEEP;
    s_dynGrantedTo = 0;
    bumpSchedule(SCHED_ALARM_RELEASE, was);
    changed = true;
  }

  /* --- 3: revoke the slot of a node that has stopped transmitting ---------- */
  for (uint8_t slot = 0; slot < TDMA_NUM_SLOTS; slot++) {
    const uint8_t own = s_sched.owner[slot];
    if (!own || own == BROADCAST_ID || own == CLUSTER_HEAD_ID || own == SINK_ID) continue;
    if (s_sched.kind[slot] != SK_DATA) continue;

    if (s_silentFor[own] >= SCHED_SILENT_LIMIT && !s_revokedOwner[slot]) {
      s_revokedOwner[slot]  = own;
      s_sched.owner[slot]   = 0;
      s_sched.kind[slot]    = SK_SLEEP;
      bumpSchedule(SCHED_NODE_SILENT, own);
      changed = true;
    }
  }

  /* --- 4: restore a slot when its owner comes back ------------------------- */
  for (uint8_t slot = 0; slot < TDMA_NUM_SLOTS; slot++) {
    const uint8_t was = s_revokedOwner[slot];
    if (!was) continue;
    if (s_silentFor[was] == 0) {
      s_sched.owner[slot]  = was;
      s_sched.kind[slot]   = SK_DATA;
      s_revokedOwner[slot] = 0;
      bumpSchedule(SCHED_NODE_REJOIN, was);
      changed = true;
    }
  }

  /* Age the silence counters once per review (one review per superframe).    */
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID) continue;
    if (s_silentFor[i] < 255) s_silentFor[i]++;
  }

  return changed;
}

void tdmaScheduleCollision(uint8_t a, uint8_t b) {
  if (NODE_ID != CLUSTER_HEAD_ID) return;
  /* Deliberately assign two owners to the contention slot.  Both will have a
   * frame ready at the same instant, which is exactly the situation the lab
   * asks us to create and explain.                                           */
  s_sched.owner[SLOT_CAP]  = a;
  s_sched.owner2[SLOT_CAP] = b;
  s_sched.kind[SLOT_CAP]   = SK_CAP;
  bumpSchedule(SCHED_COLLISION_TEST, a);
  LOGW(TAG, "COLLISION TEST ARMED: N%u and N%u both own slot %u this round",
       a, b, SLOT_CAP);
}

void tdmaBuildBeacon(BeaconPayload *b) {
  memset(b, 0, sizeof(*b));
  const uint32_t net = tdmaNetworkTimeMs();
  b->frameStartMs = net - (net % TDMA_SUPERFRAME_MS);
  b->netTimeMs    = net;
  b->schedVersion = s_sched.version;
  b->slotMs       = (uint16_t)TDMA_SLOT_MS;
  b->numSlots     = TDMA_NUM_SLOTS;
  memcpy(b->slotOwner,  s_sched.owner,  TDMA_NUM_SLOTS);
  memcpy(b->slotOwner2, s_sched.owner2, TDMA_NUM_SLOTS);
  memcpy(b->slotKind,   s_sched.kind,   TDMA_NUM_SLOTS);
  b->reason     = s_sched.reason;
  b->reasonNode = s_sched.reasonNode;
  b->chId       = CLUSTER_HEAD_ID;
  b->sinkId     = SINK_ID;
}

/* ========================================================================== */

const Schedule  *tdmaSchedule(void) { return &s_sched; }
const TdmaStats *tdmaStats(void)    { return &s_stats; }

void tdmaPrintSchedule(void) {
  char who[12] = "";
  if (s_sched.reasonNode) snprintf(who, sizeof(who), " N%u", s_sched.reasonNode);

  Serial.println();
  Serial.printf("  --- SCHEDULE v%u (%s%s) -------------------------------\r\n",
                s_sched.version, wsnSchedReasonName(s_sched.reason), who);
  Serial.println("   slot   t(ms)        kind     owner   this node");
  for (uint8_t i = 0; i < TDMA_NUM_SLOTS; i++) {
    const bool mine = tdmaIsSlotMine(i);
    char owner[16];
    if (s_sched.owner[i] == BROADCAST_ID)   strcpy(owner, "ALL");
    else if (s_sched.owner[i] == 0)         strcpy(owner, "-");
    else snprintf(owner, sizeof(owner), "N%u", s_sched.owner[i]);
    if (s_sched.owner2[i]) {
      char x[8]; snprintf(x, sizeof(x), "+N%u", s_sched.owner2[i]);
      strncat(owner, x, sizeof(owner) - strlen(owner) - 1);
    }
    Serial.printf("    %u    %4lu-%-4lu   %-8s %-7s %s\r\n",
                  i, (unsigned long)(i * TDMA_SLOT_MS),
                  (unsigned long)((i + 1) * TDMA_SLOT_MS),
                  tdmaSlotKindName(s_sched.kind[i]), owner,
                  mine ? "TRANSMIT" : (slotNeedsRadio(i) ? "listen" : "SLEEP"));
  }
  Serial.println();
}

void tdmaPrintStats(void) {
  const TdmaStats *t = &s_stats;
  Serial.println();
  Serial.println("  --- TDMA ---------------------------------------------------");
  Serial.printf("   Synced             : %s   (last correction %+ld ms)\r\n",
                s_synced ? "yes" : "NO - waiting for a beacon",
                (long)t->lastSyncOffsetMs);
  Serial.printf("   Network time       : %lu ms   superframe #%lu   slot %u\r\n",
                (unsigned long)tdmaNetworkTimeMs(),
                (unsigned long)tdmaSuperframeIndex(), tdmaCurrentSlot());
  Serial.printf("   Superframes        : %lu\r\n", (unsigned long)t->superframes);
  Serial.printf("   Schedule version   : %u  (%lu updates)\r\n",
                s_sched.version, (unsigned long)t->scheduleUpdates);
  Serial.printf("   Slots owned / used : %lu / %lu   (unused %lu)\r\n",
                (unsigned long)t->slotsOwned, (unsigned long)t->slotsUsed,
                (unsigned long)t->slotsUnused);
  Serial.printf("   Beacons rx/missed  : %lu / %lu\r\n",
                (unsigned long)t->beaconsRx, (unsigned long)t->beaconsMissed);
  Serial.printf("   Sleep slots        : %lu   (%lu ms total)\r\n",
                (unsigned long)t->sleepSlots, (unsigned long)t->sleepMs);
  Serial.printf("   Radio duty cycle   : %.1f %%  (sleep mode: %s)\r\n",
                tdmaDutyCyclePct(),
                ENABLE_LIGHT_SLEEP ? "light sleep" : "radio off");
  Serial.println();
}
