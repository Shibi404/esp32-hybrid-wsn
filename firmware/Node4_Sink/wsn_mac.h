/* =============================================================================
 *  wsn_mac.h  --  Application-layer CSMA/CA over ESP-NOW.
 *
 *  ESP-NOW rides on 802.11, which already has its own CSMA/CA in silicon.  We
 *  deliberately implement a second, visible one on top of it, because the point
 *  of the exercise is to *demonstrate* the mechanism: every carrier-sense
 *  decision, backoff counter, ACK timeout and contention-window doubling is
 *  logged and surfaced on the dashboard.  Hiding it inside the chip would
 *  prove nothing.
 *
 *      have frame
 *          |
 *    [1] CHANNEL SENSE -- two independent mechanisms, OR-ed:
 *          * PHYSICAL  promiscuous-mode sniffer timestamps every 802.11 frame
 *                      it sees; the medium counts busy for CCA_BUSY_WINDOW_US
 *                      after any frame stronger than CCA_RSSI_THRESHOLD.
 *          * VIRTUAL   every WSN frame header carries durationUs; a node that
 *                      overhears one sets its NAV and treats the medium as
 *                      busy for that long.  (802.11's RTS/CTS idea.)
 *          |
 *    [2] DIFS -- the medium must stay idle for MAC_DIFS_US continuously
 *          |
 *    [3] RANDOM BACKOFF -- backoff = random(0, CW) slots of MAC_SLOT_US.
 *          The counter decrements only while the medium is idle and FREEZES
 *          when it goes busy.  This freeze is what actually resolves
 *          contention between two nodes that started together.
 *          |
 *    [4] TRANSMIT
 *          |
 *    [5] WAIT FOR ACK (MAC_ACK_TIMEOUT_MS)
 *          ACK      -> CW := CW_MIN, record RTT, credit the link
 *          timeout  -> retransmit, CW := min(2*CW, CW_MAX)   (binary
 *                      exponential backoff), up to MAC_MAX_RETRIES, then drop
 *                      and penalise the link so routing can react.
 *
 *  Broadcasts are not acknowledged -- matching 802.11 semantics -- and use a
 *  shorter contention window.
 * ========================================================================== */

#ifndef WSN_MAC_H
#define WSN_MAC_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef enum : uint8_t {
  MAC_IDLE = 0,     /* nothing queued                                        */
  MAC_SENSE,        /* waiting for the medium to go idle for DIFS            */
  MAC_BACKOFF,      /* counting down backoff slots                           */
  MAC_TX,           /* handing the frame to the radio                        */
  MAC_WAIT_ACK      /* awaiting the application-layer ACK                    */
} MacState;

typedef struct {
  uint32_t txAttempts;      /* transmissions handed to the radio             */
  uint32_t txSuccess;       /* unicasts that were acknowledged               */
  uint32_t txDropped;       /* gave up after MAC_MAX_RETRIES                 */
  uint32_t retransmissions; /* retries beyond the first attempt              */
  uint32_t ackTimeouts;
  uint32_t broadcastsSent;
  uint32_t rxFrames;
  uint32_t rxBadCrc;
  uint32_t ccaBusyDeferrals; /* times the medium was found busy              */
  uint32_t backoffFreezes;   /* times a running backoff was frozen           */
  uint32_t forcedTx;         /* medium busy longer than MAC_SENSE_TIMEOUT_MS */
  uint32_t queueDrops;       /* enqueue failed, queue full                   */
  uint32_t totalBackoffSlots;
  uint32_t ackRttSumMs;
  uint32_t ackRttCount;
  uint16_t lastBackoff;
  uint16_t lastCw;
  uint32_t lastAckRttMs;
} MacStats;

/* Called for every validated inbound frame.                                  */
typedef void (*MacRxHandler)(const WsnHeader *h, const uint8_t *payload,
                             size_t payloadLen, int8_t rssi);

bool macBegin(MacRxHandler handler);
void macLoop(void);
/* Re-register the ESP-NOW peers.  esp_wifi_stop() during a sleep slot can
 * tear the peer list down, and a send to a peer that is no longer registered
 * fails silently, so the scheduler calls this after every radio wake.       */
void macEnsurePeers(void);

/* Queue a frame.  `seq` is assigned here for originated frames (seq == 0).
 * Returns false if the queue is full.                                        */
bool macSend(uint8_t type, uint8_t dst, uint8_t finalDst,
             const void *payload, size_t payloadLen);
/* Forward an already-formed frame to a new next hop, preserving src/seq.     */
bool macForward(const WsnHeader *h, const void *payload, size_t payloadLen,
                uint8_t nextHop);

bool     macIsChannelBusy(void);
/* Wi-Fi RSSI last observed from a peer, or 0 if it has not been heard
 * recently.  Harvested by the promiscuous sniffer, so it is available on both
 * Arduino core 2.x and 3.x despite their different receive-callback
 * signatures.  The routing layer uses it as its link-quality input.          */
int8_t   macPeerRssi(uint8_t id);
MacState macGetState(void);
uint16_t macGetCw(void);
uint8_t  macQueueLen(void);

const MacStats *macStats(void);
void  macResetStats(void);
void  macPrintStats(void);
float macAckRate(void);
uint32_t macAvgAckRttMs(void);

/* --- demonstration hooks -------------------------------------------------- */
/* Pretend the link to `id` is broken: frames to it are transmitted but its
 * ACKs are ignored, so the sender observes real timeouts and real route
 * recovery.  Modelling the failure at the receiver's ACK rather than skipping
 * the transmission keeps the MAC's retry path genuinely exercised.           */
void macSimulateLinkFailure(uint8_t id, bool fail);
bool macLinkSimulatedDown(uint8_t id);
/* Bypass carrier sense for the next `n` transmissions -- used by `collide
 * hard` to force an actual overlap and show the ACK-timeout recovery path.   */
void macForceCollision(uint8_t n);

/* Node id whose ACK we are waiting for, or 0.                                */
uint8_t macPendingPeer(void);

#endif /* WSN_MAC_H */
