/* =============================================================================
 *  wsn_mac.cpp  --  CSMA/CA state machine over ESP-NOW.
 * ========================================================================== */

#include "wsn_mac.h"
#include "wsn_log.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define TAG "MAC"

/* --- transmit queue ------------------------------------------------------- */
typedef struct {
  WsnHeader h;
  uint8_t   payload[80];
  size_t    payloadLen;
  uint8_t   retries;
  bool      needsAck;
  bool      inUse;
  uint32_t  enqueuedMs;
  uint32_t  firstTxMs;   /* first attempt, for the ACK round-trip time        */
} TxItem;

static TxItem  s_txq[MAC_TXQ_DEPTH];
static uint8_t s_txHead = 0, s_txTail = 0, s_txCount = 0;

/* --- receive ring (filled in the Wi-Fi task, drained in macLoop) ---------- */
#define RXQ_DEPTH 10
typedef struct {
  uint8_t  buf[WSN_MAX_FRAME];
  uint16_t len;
  int8_t   rssi;
} RxItem;

static volatile RxItem  s_rxq[RXQ_DEPTH];
static volatile uint8_t s_rxHead = 0, s_rxTail = 0;

/* --- pending ACKs we owe (sent after SIFS, outside the Wi-Fi task) -------- */
#define ACKQ_DEPTH 4
typedef struct { uint8_t to; uint8_t src; uint16_t seq; int8_t rssi; uint32_t dueUs; } AckItem;
static AckItem  s_ackq[ACKQ_DEPTH];
static uint8_t  s_ackCount = 0;

/* --- state ---------------------------------------------------------------- */
static MacState     s_state      = MAC_IDLE;
static MacStats     s_stats;
static MacRxHandler s_handler    = nullptr;
static uint16_t     s_cw         = MAC_CW_MIN;
static uint16_t     s_backoff    = 0;
static uint32_t     s_lastSlotUs = 0;
static uint32_t     s_senseStartMs = 0;
static uint32_t     s_idleSinceUs  = 0;
static uint32_t     s_ackDeadlineMs = 0;
static uint16_t     s_seqCounter  = 0;   /* control frames                  */
static uint16_t     s_dataSeq     = 0;   /* data-bearing frames only        */
static bool         s_ready      = false;

/* --- carrier sense -------------------------------------------------------- */
static volatile uint32_t s_lastAirUs = 0;      /* last 802.11 frame heard     */
static volatile uint32_t s_navUntilUs = 0;     /* virtual carrier sense       */
static volatile int8_t   s_peerRssi[NUM_NODES + 1];
static volatile uint32_t s_peerRssiMs[NUM_NODES + 1];

/* --- demonstration hooks -------------------------------------------------- */
static bool    s_simFailedPeer[NUM_NODES + 1] = { false };
static uint8_t s_forceCollision = 0;

/* ========================================================================== */
/*  Promiscuous-mode sniffer: physical carrier sense + per-peer Wi-Fi RSSI     */
/* ========================================================================== */

#if ENABLE_PHY_CCA
static void IRAM_ATTR snifferCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
  if (!pkt) return;

  const int rssi = pkt->rx_ctrl.rssi;

  /* Physical CCA: anything above the threshold means the medium is occupied.
   * Weaker frames are treated as noise, exactly as a real CCA threshold does. */
  if (rssi > CCA_RSSI_THRESHOLD) s_lastAirUs = micros();

  /* Opportunistically harvest the Wi-Fi RSSI of our own peers.  addr2 (the
   * transmitter address) sits at offset 10 of the 802.11 MAC header.  Doing it
   * here means per-peer Wi-Fi RSSI works on both Arduino core 2.x and 3.x,
   * where the ESP-NOW receive callback signatures differ.                    */
  if (pkt->rx_ctrl.sig_len >= 16) {
    const uint8_t *a2 = pkt->payload + 10;
    for (uint8_t i = 1; i <= NUM_NODES; i++) {
      if (memcmp(a2, NODE_MAC[i], 6) == 0) {
        s_peerRssi[i]   = (int8_t)rssi;
        s_peerRssiMs[i] = millis();
        break;
      }
    }
  }
}
#endif

bool macIsChannelBusy(void) {
  const uint32_t now = micros();
#if ENABLE_PHY_CCA
  if ((uint32_t)(now - s_lastAirUs) < CCA_BUSY_WINDOW_US) return true;
#endif
  /* Virtual carrier sense: a NAV set by an overheard frame's duration field. */
  if ((int32_t)(s_navUntilUs - now) > 0) return true;
  return false;
}

int8_t macPeerRssi(uint8_t id) {
  if (id < 1 || id > NUM_NODES) return 0;
  if (millis() - s_peerRssiMs[id] > NEIGHBOR_TIMEOUT_MS) return 0;
  return s_peerRssi[id];
}

/* ========================================================================== */
/*  ESP-NOW receive                                                           */
/* ========================================================================== */

static void pushRx(const uint8_t *data, int len, int8_t rssi) {
  const uint8_t next = (uint8_t)((s_rxHead + 1) % RXQ_DEPTH);
  if (next == s_rxTail) return;            /* ring full -- drop oldest-first  */
  if (len <= 0 || len > WSN_MAX_FRAME) return;

  memcpy((void *)s_rxq[s_rxHead].buf, data, (size_t)len);
  s_rxq[s_rxHead].len  = (uint16_t)len;
  s_rxq[s_rxHead].rssi = rssi;
  s_rxHead = next;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  int8_t rssi = 0;
  if (info && info->rx_ctrl) rssi = (int8_t)info->rx_ctrl->rssi;
  if (rssi == 0 && info && info->src_addr) rssi = macPeerRssi(wsnNodeIdFromMac(info->src_addr));
  pushRx(data, len, rssi);
}
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  /* Core 2.x gives no RSSI here; the promiscuous sniffer supplies it.        */
  pushRx(data, len, macPeerRssi(wsnNodeIdFromMac(mac)));
}
#endif

/* No send callback is registered on purpose.  Its signature changed between
 * ESP-IDF 5.3 and 5.4 (mac_addr vs wifi_tx_info_t), and we do not need it:
 * esp_now_send() reports queueing errors synchronously, and delivery is
 * confirmed by our own application-layer ACK -- which is the thing the lab
 * asks us to implement and measure.                                          */

/* ========================================================================== */
/*  Peers                                                                     */
/* ========================================================================== */

static bool addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = WSN_WIFI_CHANNEL;
  p.encrypt = false;
  p.ifidx   = WIFI_IF_STA;

  const esp_err_t e = esp_now_add_peer(&p);
  if (e != ESP_OK) {
    char b[18];
    LOGE(TAG, "esp_now_add_peer(%s) failed: %s", logMacStr(mac, b), esp_err_to_name(e));
    return false;
  }
  return true;
}

/* Re-register every peer.  Called at start-up and again after waking from a
 * radio-off sleep, where the Wi-Fi stack may have been torn down.            */
static void ensurePeers(void) {
  addPeer(BROADCAST_MAC);
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID) continue;
    addPeer(NODE_MAC[i]);
  }
}

/* ========================================================================== */
/*  Init                                                                      */
/* ========================================================================== */

bool macBegin(MacRxHandler handler) {
  s_handler = handler;
  memset(&s_stats, 0, sizeof(s_stats));
  memset(s_txq, 0, sizeof(s_txq));
  s_txHead = s_txTail = s_txCount = 0;
  s_rxHead = 0;
  s_rxTail = 0;
  s_ackCount = 0;
  s_cw = MAC_CW_MIN;
  s_state = MAC_IDLE;

  if (esp_now_init() != ESP_OK) {
    LOGE(TAG, "esp_now_init() failed");
    return false;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  ensurePeers();

#if ENABLE_PHY_CCA
  /* Listen to management and data frames.  Control frames are short and very
   * frequent; including them would make the medium look permanently busy.   */
  wifi_promiscuous_filter_t filt;
  memset(&filt, 0, sizeof(filt));
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&snifferCb);
  if (esp_wifi_set_promiscuous(true) != ESP_OK) {
    LOGW(TAG, "Promiscuous mode unavailable - falling back to virtual (NAV) "
              "carrier sense only");
  } else {
    LOGI(TAG, "Physical CCA active (threshold %d dBm, busy window %d us)",
         CCA_RSSI_THRESHOLD, CCA_BUSY_WINDOW_US);
  }
#else
  LOGI(TAG, "Physical CCA disabled - using virtual (NAV) carrier sense only");
#endif

  s_idleSinceUs = micros();
  s_ready = true;
  LOGI(TAG, "CSMA/CA up: slot %d us, DIFS %d us, CW %d..%d, ACK timeout %d ms, "
            "max %d retries", MAC_SLOT_US, MAC_DIFS_US, MAC_CW_MIN, MAC_CW_MAX,
       MAC_ACK_TIMEOUT_MS, MAC_MAX_RETRIES);
  return true;
}

/* ========================================================================== */
/*  Enqueue                                                                   */
/* ========================================================================== */

static TxItem *queuePush(void) {
  if (s_txCount >= MAC_TXQ_DEPTH) return nullptr;
  TxItem *it = &s_txq[s_txHead];
  s_txHead = (uint8_t)((s_txHead + 1) % MAC_TXQ_DEPTH);
  s_txCount++;
  memset(it, 0, sizeof(*it));
  it->inUse      = true;
  it->enqueuedMs = millis();
  return it;
}

static TxItem *queueFront(void) {
  return s_txCount ? &s_txq[s_txTail] : nullptr;
}

static void queuePop(void) {
  if (!s_txCount) return;
  s_txq[s_txTail].inUse = false;
  s_txTail = (uint8_t)((s_txTail + 1) % MAC_TXQ_DEPTH);
  s_txCount--;
}

uint8_t macQueueLen(void) { return s_txCount; }

/* Rough on-air time, used for the NAV duration field other nodes will honour.
 * ESP-NOW action frames go out at the basic rate, so ~10 us/byte plus the
 * preamble and inter-frame overhead is a fair estimate.                      */
static uint16_t estimateDurationUs(size_t frameLen, bool needsAck) {
  uint32_t d = 250 + frameLen * 10;
  if (needsAck) d += MAC_SIFS_US + 350;
  return (uint16_t)(d > 65535 ? 65535 : d);
}

bool macSend(uint8_t type, uint8_t dst, uint8_t finalDst,
             const void *payload, size_t payloadLen) {
  if (!s_ready) return false;
  if (payloadLen > sizeof(((TxItem *)0)->payload)) {
    LOGE(TAG, "payload %u B exceeds the %u B queue slot", (unsigned)payloadLen,
         (unsigned)sizeof(((TxItem *)0)->payload));
    return false;
  }

  TxItem *it = queuePush();
  if (!it) {
    s_stats.queueDrops++;
    LOGW(TAG, "TX queue full (%d) - dropping %s to N%u",
         MAC_TXQ_DEPTH, wsnFrameTypeName(type), dst);
    return false;
  }

  wsnInitHeader(&it->h, type, NODE_ID, dst, finalDst);

  /* Data-bearing frames get their own sequence space.  The sink derives PDR
   * and loss from gaps in a source's sequence numbers, so if HELLOs and
   * beacons shared the counter every data frame would appear to be preceded
   * by several lost ones and PDR would read far below its true value.
   * Only one unicast is ever in flight per node, so the two spaces cannot
   * make an ACK ambiguous.                                                  */
  it->h.seq = (type == FT_DATA || type == FT_AGG) ? ++s_dataSeq : ++s_seqCounter;
  if (payloadLen && payload) memcpy(it->payload, payload, payloadLen);
  it->payloadLen = payloadLen;
  it->needsAck   = (dst != BROADCAST_ID) && (type != FT_ACK);
  return true;
}

bool macForward(const WsnHeader *h, const void *payload, size_t payloadLen,
                uint8_t nextHop) {
  if (!s_ready) return false;
  if (payloadLen > sizeof(((TxItem *)0)->payload)) return false;

  if (h->ttl == 0) {
    LOGW(TAG, "TTL expired for %s seq=%u from N%u - dropping",
         wsnFrameTypeName(h->type), h->seq, h->src);
    return false;
  }
  /* Loop guard: never hand a frame to a node that already carried it.        */
  if (wsnPathContains(h, nextHop)) {
    LOGW(TAG, "Refusing to forward seq=%u to N%u - already in the path",
         h->seq, nextHop);
    return false;
  }

  TxItem *it = queuePush();
  if (!it) { s_stats.queueDrops++; return false; }

  it->h = *h;                         /* preserve src, seq and txTimeMs       */
  it->h.dst = nextHop;
  it->h.hop = (uint8_t)(h->hop + 1);
  it->h.ttl = (uint8_t)(h->ttl - 1);
  wsnPathAppend(&it->h, NODE_ID);

  if (payloadLen && payload) memcpy(it->payload, payload, payloadLen);
  it->payloadLen = payloadLen;
  it->needsAck   = (nextHop != BROADCAST_ID);
  return true;
}

/* ========================================================================== */
/*  Transmit                                                                  */
/* ========================================================================== */

extern uint32_t tdmaNetworkTimeMs(void);   /* from wsn_tdma.cpp               */

static bool rawSend(TxItem *it) {
  uint8_t frame[WSN_MAX_FRAME];

  /* Stamp the origination time only on the first attempt of an originated
   * frame.  Re-stamping on a retransmission would hide the retry's cost from
   * the end-to-end delay measurement, which is exactly what we want to see.  */
  if (it->h.txTimeMs == 0) it->h.txTimeMs = tdmaNetworkTimeMs();

  const size_t probe = WSN_HDR_LEN + it->payloadLen + 2;
  it->h.durationUs = estimateDurationUs(probe, it->needsAck);

  const size_t len = wsnSerialize(frame, sizeof(frame), &it->h,
                                  it->payload, it->payloadLen);
  if (!len) { LOGE(TAG, "serialise failed"); return false; }

  const uint8_t *mac = wsnMacForNode(it->h.dst);
  if (!mac) { LOGE(TAG, "no MAC for N%u", it->h.dst); return false; }

  const esp_err_t e = esp_now_send(mac, frame, len);
  if (e != ESP_OK) {
    /* ESP_ERR_ESPNOW_NOT_INIT shows up after a radio-off sleep tore the stack
     * down; re-registering the peers and retrying next pass recovers it.     */
    LOGW(TAG, "esp_now_send -> %s", esp_err_to_name(e));
    ensurePeers();
    return false;
  }

  /* Our own transmission occupies the medium too: set the local NAV so the
   * state machine does not immediately think the channel is free.            */
  s_navUntilUs = micros() + it->h.durationUs;

  s_stats.txAttempts++;
  if (it->h.dst == BROADCAST_ID) s_stats.broadcastsSent++;

  char path[20];
  wsnPathToString(&it->h, path, sizeof(path));
  LOGI(TAG, "TX %-7s seq=%-4u %s N%u->N%u (final N%u, hop %u, %u B, path %s)%s",
       wsnFrameTypeName(it->h.type), it->h.seq,
       it->h.dst == BROADCAST_ID ? "BROADCAST" : "UNICAST",
       it->h.src, it->h.dst, it->h.finalDst, it->h.hop, (unsigned)len, path,
       it->retries ? String(String("  [retry ") + it->retries + "]").c_str() : "");
  logCsv("TX,%s,%u,%u,%u,%u,%u,%u", wsnFrameTypeName(it->h.type), it->h.seq,
         it->h.src, it->h.dst, it->h.finalDst, (unsigned)len, it->retries);
  return true;
}

/* ========================================================================== */
/*  Receive processing                                                        */
/* ========================================================================== */

static void queueAck(uint8_t to, uint8_t src, uint16_t seq, int8_t rssi) {
  if (s_ackCount >= ACKQ_DEPTH) return;
  s_ackq[s_ackCount].to    = to;
  s_ackq[s_ackCount].src   = src;
  s_ackq[s_ackCount].seq   = seq;
  s_ackq[s_ackCount].rssi  = rssi;
  /* 802.11 returns an ACK after SIFS, without contending -- so do we.        */
  s_ackq[s_ackCount].dueUs = micros() + MAC_SIFS_US;
  s_ackCount++;
}

static void sendDueAcks(void) {
  const uint32_t now = micros();
  uint8_t kept = 0;

  for (uint8_t i = 0; i < s_ackCount; i++) {
    if ((int32_t)(s_ackq[i].dueUs - now) > 0) { s_ackq[kept++] = s_ackq[i]; continue; }

    WsnHeader h;
    AckPayload a;
    wsnInitHeader(&h, FT_ACK, NODE_ID, s_ackq[i].to, s_ackq[i].to);
    h.seq        = s_ackq[i].seq;        /* echo the sequence being ACKed     */
    h.durationUs = 300;
    a.ackSeq = s_ackq[i].seq;
    a.ackSrc = s_ackq[i].src;
    a.rssi   = s_ackq[i].rssi;

    uint8_t frame[WSN_MAX_FRAME];
    const size_t len = wsnSerialize(frame, sizeof(frame), &h, &a, sizeof(a));
    const uint8_t *mac = wsnMacForNode(s_ackq[i].to);
    if (len && mac) {
      esp_now_send(mac, frame, len);
      LOGD(TAG, "ACK -> N%u for seq=%u (rssi %d dBm)",
           s_ackq[i].to, s_ackq[i].seq, s_ackq[i].rssi);
    }
  }
  s_ackCount = kept;
}

static void processRx(void) {
  while (s_rxTail != s_rxHead) {
    RxItem item;
    memcpy(&item, (const void *)&s_rxq[s_rxTail], sizeof(item));
    s_rxTail = (uint8_t)((s_rxTail + 1) % RXQ_DEPTH);

    WsnHeader h;
    const uint8_t *pl;
    size_t plen;

    if (!wsnParse(item.buf, item.len, &h, &pl, &plen)) {
      s_stats.rxBadCrc++;
      LOGW(TAG, "Dropped a malformed/corrupt frame (%u B)", item.len);
      continue;
    }

    s_stats.rxFrames++;

    /* Virtual carrier sense: honour the sender's declared medium occupancy.  */
    if (h.durationUs) s_navUntilUs = micros() + h.durationUs;

    /* Ignore our own broadcasts echoed back.                                 */
    if (h.src == NODE_ID) continue;

    /* ----- ACK matching ---------------------------------------------------- */
    if (h.type == FT_ACK) {
      if (plen < sizeof(AckPayload)) continue;
      const AckPayload *a = (const AckPayload *)pl;

      /* A simulated link failure is modelled by discarding the peer's ACK, so
       * the sender runs its real timeout, retry and route-recovery path.     */
      if (h.src <= NUM_NODES && s_simFailedPeer[h.src]) {
        LOGD(TAG, "Ignoring ACK from N%u (simulated link failure)", h.src);
        continue;
      }

      TxItem *f = queueFront();
      if (s_state == MAC_WAIT_ACK && f && a->ackSeq == f->h.seq && h.src == f->h.dst) {
        const uint32_t rtt = millis() - f->firstTxMs;
        s_stats.txSuccess++;
        s_stats.ackRttSumMs += rtt;
        s_stats.ackRttCount++;
        s_stats.lastAckRttMs = rtt;

        LOGI(TAG, "ACK  from N%u for seq=%-4u (%lu ms, %u attempt%s) - delivered",
             h.src, a->ackSeq, (unsigned long)rtt,
             f->retries + 1, f->retries ? "s" : "");
        logCsv("ACK,%u,%u,%lu,%u", h.src, a->ackSeq, (unsigned long)rtt, f->retries);

        extern void routeReportTxResult(uint8_t peer, bool ok);
        routeReportTxResult(h.src, true);

        if (s_handler) s_handler(&h, pl, plen, item.rssi);
        queuePop();
        s_cw    = MAC_CW_MIN;      /* success resets the contention window    */
        s_state = MAC_IDLE;
        s_idleSinceUs = micros();
      }
      continue;
    }

    /* ----- everything else -------------------------------------------------- */
    /* Accept broadcasts, and unicasts addressed to this node.                */
    if (h.dst != BROADCAST_ID && h.dst != NODE_ID) continue;

    /* Acknowledge unicast data-bearing frames before handing them upward, so
     * the ACK is not delayed by application processing.                      */
    if (h.dst == NODE_ID &&
        (h.type == FT_DATA || h.type == FT_AGG || h.type == FT_LOC ||
         h.type == FT_SLOT_REQ)) {
      queueAck(h.src, h.src, h.seq, item.rssi);
    }

    if (s_handler) s_handler(&h, pl, plen, item.rssi);
  }
}

/* ========================================================================== */
/*  State machine                                                             */
/* ========================================================================== */

void macLoop(void) {
  if (!s_ready) return;

  processRx();
  sendDueAcks();

  const uint32_t nowUs = micros();
  const bool busy = macIsChannelBusy();
  /* s_idleSinceUs marks the start of the current continuously-idle period;
   * every busy observation restarts it, which is what makes the DIFS check
   * below a *continuous* idle requirement rather than an instantaneous one. */
  if (busy) s_idleSinceUs = nowUs;

  TxItem *it = queueFront();

  switch (s_state) {

    case MAC_IDLE:
      if (!it) return;
      s_senseStartMs = millis();
      s_state = MAC_SENSE;
      LOGD(TAG, "Frame queued (%s seq=%u) - starting carrier sense",
           wsnFrameTypeName(it->h.type), it->h.seq);
      /* fall through on the next pass */
      break;

    /* --- [1]/[2] channel sense + DIFS ------------------------------------- */
    case MAC_SENSE: {
      if (!it) { s_state = MAC_IDLE; return; }

      /* `collide hard` bypasses carrier sense to force a genuine overlap, so
       * the ACK-timeout and exponential-backoff recovery path can be shown.  */
      if (s_forceCollision) {
        s_forceCollision--;
        LOGW(TAG, "COLLISION TEST: bypassing carrier sense for this frame "
                  "(%u remaining)", s_forceCollision);
        s_backoff = 0;
        s_state   = MAC_TX;
        return;
      }

      if (busy) {
        s_stats.ccaBusyDeferrals++;
        LOGD(TAG, "CCA: medium BUSY - deferring");

        /* Do not block forever on a saturated band: after MAC_SENSE_TIMEOUT_MS
         * transmit anyway and count it, rather than starving the node.       */
        if (millis() - s_senseStartMs > MAC_SENSE_TIMEOUT_MS) {
          s_stats.forcedTx++;
          LOGW(TAG, "Medium busy for >%d ms - transmitting anyway (forced)",
               MAC_SENSE_TIMEOUT_MS);
          s_backoff = 0;
          s_state   = MAC_TX;
        }
        return;
      }

      /* Medium must be continuously idle for a full DIFS.                    */
      if ((uint32_t)(nowUs - s_idleSinceUs) < MAC_DIFS_US) return;

      /* --- [3] random backoff ---------------------------------------------- */
      s_backoff = (uint16_t)(esp_random() % s_cw);
      s_stats.lastBackoff = s_backoff;
      s_stats.lastCw      = s_cw;
      s_stats.totalBackoffSlots += s_backoff;
      s_lastSlotUs = nowUs;
      s_state = MAC_BACKOFF;

      LOGI(TAG, "CCA idle for DIFS -> backoff = %u slots (CW=%u, slot=%d us)",
           s_backoff, s_cw, MAC_SLOT_US);
      logCsv("BACKOFF,%u,%u", s_backoff, s_cw);
      return;
    }

    /* --- [3] backoff countdown with freeze -------------------------------- */
    case MAC_BACKOFF: {
      if (!it) { s_state = MAC_IDLE; return; }

      if (busy) {
        /* THIS is how contention is resolved: the loser freezes its counter
         * mid-countdown and resumes from where it stopped, so it does not
         * start over and does not collide again with the winner.             */
        s_stats.backoffFreezes++;
        LOGI(TAG, "Medium went BUSY - backoff FROZEN at %u slots "
                  "(will resume, not restart)", s_backoff);
        s_state = MAC_SENSE;
        return;
      }

      if ((uint32_t)(nowUs - s_lastSlotUs) < MAC_SLOT_US) return;
      s_lastSlotUs = nowUs;

      if (s_backoff > 0) { s_backoff--; return; }
      s_state = MAC_TX;
      return;
    }

    /* --- [4] transmit ----------------------------------------------------- */
    case MAC_TX: {
      if (!it) { s_state = MAC_IDLE; return; }

      if (it->firstTxMs == 0) it->firstTxMs = millis();
      if (!rawSend(it)) {
        /* Could not hand it to the radio at all; treat as a failed attempt.  */
        it->retries++;
        if (it->retries > MAC_MAX_RETRIES) { queuePop(); s_state = MAC_IDLE; }
        else                               { s_state = MAC_SENSE; }
        return;
      }

      if (!it->needsAck) {
        queuePop();                 /* broadcasts are fire-and-forget         */
        s_state = MAC_IDLE;
        s_idleSinceUs = micros();
        return;
      }

      s_ackDeadlineMs = millis() + MAC_ACK_TIMEOUT_MS;
      s_state = MAC_WAIT_ACK;
      return;
    }

    /* --- [5] wait for ACK, retransmit on timeout --------------------------- */
    case MAC_WAIT_ACK: {
      if (!it) { s_state = MAC_IDLE; return; }
      if ((int32_t)(millis() - s_ackDeadlineMs) < 0) return;

      s_stats.ackTimeouts++;
      it->retries++;

      if (it->retries > MAC_MAX_RETRIES) {
        s_stats.txDropped++;
        LOGE(TAG, "No ACK from N%u for seq=%u after %d attempts - DROPPING "
                  "(link will be penalised)", it->h.dst, it->h.seq,
             MAC_MAX_RETRIES + 1);
        logCsv("DROP,%u,%u,%u", it->h.dst, it->h.seq, it->retries);

        /* Tell the routing layer, so it can mark the link down and re-route. */
        extern void routeReportTxResult(uint8_t peer, bool ok);
        routeReportTxResult(it->h.dst, false);

        queuePop();
        s_cw    = MAC_CW_MIN;
        s_state = MAC_IDLE;
        return;
      }

      /* Binary exponential backoff: every failure doubles the window, which
       * spreads retrying nodes further apart in time.                        */
      const uint16_t oldCw = s_cw;
      s_cw = (uint16_t)min((int)(s_cw * 2), (int)MAC_CW_MAX);
      s_stats.retransmissions++;

      LOGW(TAG, "ACK TIMEOUT for seq=%u to N%u (%d ms) - retry %u/%d, "
                "CW %u -> %u (binary exponential backoff)",
           it->h.seq, it->h.dst, MAC_ACK_TIMEOUT_MS, it->retries,
           MAC_MAX_RETRIES, oldCw, s_cw);
      logCsv("RETX,%u,%u,%u,%u", it->h.dst, it->h.seq, it->retries, s_cw);

      extern void routeReportTxResult(uint8_t peer, bool ok);
      routeReportTxResult(it->h.dst, false);

      s_senseStartMs = millis();
      s_state = MAC_SENSE;
      return;
    }
  }
}

/* ========================================================================== */
/*  Accessors and demo hooks                                                  */
/* ========================================================================== */

MacState macGetState(void)  { return s_state; }
uint16_t macGetCw(void)     { return s_cw; }
uint8_t  macPendingPeer(void) {
  TxItem *f = queueFront();
  return (s_state == MAC_WAIT_ACK && f) ? f->h.dst : 0;
}

const MacStats *macStats(void) { return &s_stats; }

void macResetStats(void) {
  memset(&s_stats, 0, sizeof(s_stats));
  LOGI(TAG, "MAC statistics reset");
}

float macAckRate(void) {
  const uint32_t unicast = s_stats.txAttempts - s_stats.broadcastsSent;
  if (!unicast) return 1.0f;
  return (float)s_stats.txSuccess / (float)unicast;
}

uint32_t macAvgAckRttMs(void) {
  return s_stats.ackRttCount ? (s_stats.ackRttSumMs / s_stats.ackRttCount) : 0;
}

void macSimulateLinkFailure(uint8_t id, bool fail) {
  if (id < 1 || id > NUM_NODES) return;
  s_simFailedPeer[id] = fail;
  LOGW(TAG, "Simulated link to N%u is now %s", id, fail ? "DOWN" : "UP");
}

bool macLinkSimulatedDown(uint8_t id) {
  return (id >= 1 && id <= NUM_NODES) ? s_simFailedPeer[id] : false;
}

void macForceCollision(uint8_t n) {
  s_forceCollision = n;
  LOGW(TAG, "Carrier sense will be bypassed for the next %u transmission(s)", n);
}

void macPrintStats(void) {
  const MacStats *m = &s_stats;
  const uint32_t unicast = m->txAttempts - m->broadcastsSent;

  Serial.println();
  Serial.println("  --- MAC (CSMA/CA) ------------------------------------------");
  Serial.printf("   State              : %s   CW=%u   queue=%u\r\n",
                s_state == MAC_IDLE     ? "IDLE"
              : s_state == MAC_SENSE    ? "SENSE"
              : s_state == MAC_BACKOFF  ? "BACKOFF"
              : s_state == MAC_TX       ? "TX" : "WAIT_ACK",
                s_cw, s_txCount);
  Serial.printf("   Medium             : %s\r\n", macIsChannelBusy() ? "BUSY" : "idle");
  Serial.printf("   TX attempts        : %lu  (unicast %lu, broadcast %lu)\r\n",
                (unsigned long)m->txAttempts, (unsigned long)unicast,
                (unsigned long)m->broadcastsSent);
  Serial.printf("   Acknowledged       : %lu\r\n", (unsigned long)m->txSuccess);
  Serial.printf("   Retransmissions    : %lu\r\n", (unsigned long)m->retransmissions);
  Serial.printf("   ACK timeouts       : %lu\r\n", (unsigned long)m->ackTimeouts);
  Serial.printf("   Dropped (max retry): %lu\r\n", (unsigned long)m->txDropped);
  Serial.printf("   ACK success rate   : %.1f %%\r\n", macAckRate() * 100.0f);
  Serial.printf("   Mean ACK RTT       : %lu ms (last %lu ms)\r\n",
                (unsigned long)macAvgAckRttMs(), (unsigned long)m->lastAckRttMs);
  Serial.printf("   CCA busy deferrals : %lu\r\n", (unsigned long)m->ccaBusyDeferrals);
  Serial.printf("   Backoff freezes    : %lu   <-- contention actually resolved\r\n",
                (unsigned long)m->backoffFreezes);
  Serial.printf("   Mean backoff       : %.2f slots (last %u, CW %u)\r\n",
                m->txAttempts ? (float)m->totalBackoffSlots / m->txAttempts : 0.0f,
                m->lastBackoff, m->lastCw);
  Serial.printf("   Forced TX (busy)   : %lu\r\n", (unsigned long)m->forcedTx);
  Serial.printf("   RX frames          : %lu  (bad CRC %lu)\r\n",
                (unsigned long)m->rxFrames, (unsigned long)m->rxBadCrc);
  Serial.printf("   Queue drops        : %lu\r\n", (unsigned long)m->queueDrops);
  Serial.println();
}
