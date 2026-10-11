/* =============================================================================
 *  wsn_ble.h  --  BLE neighbour discovery, RSSI ranging and localization.
 *
 *  Implements the full chain the lab guidelines require:
 *
 *     advertise + scan  ->  neighbour discovery
 *            |
 *            v
 *     per-neighbour RSSI samples  ->  median  ->  EWMA        (noise rejection)
 *            |
 *            v
 *     log-distance path loss model  ->  estimated distance (m)
 *            |
 *            +--> argmax RSSI       ->  nearest-node / proximity
 *            |
 *            v
 *     least-squares trilateration from 3 anchors  ->  estimated (x, y)
 *            |
 *            v
 *     || estimated - ground truth ||   ->  localization error, mean, RMSE
 *
 *  Why median-then-EWMA rather than a plain average: BLE RSSI on a crowded
 *  2.4 GHz band shows occasional 15-20 dB outliers from multipath and from
 *  Wi-Fi interference. A mean is dragged by those; a median ignores them. The
 *  EWMA then smooths across localization rounds so the position estimate does
 *  not jitter between frames.
 * ========================================================================== */

#ifndef WSN_BLE_H
#define WSN_BLE_H

#include <Arduino.h>
#include "wsn_config.h"
#include "wsn_protocol.h"

typedef struct {
  bool     seen;                 /* heard at least once this round            */
  uint8_t  samples;              /* RSSI samples collected this round         */
  int8_t   rssiRaw;              /* last raw sample                           */
  int8_t   rssiMedian;           /* median of this round's samples            */
  float    rssiFiltered;         /* EWMA across rounds                        */
  float    distanceM;            /* from the path-loss model                  */
  float    trueDistanceM;        /* from the anchor coordinates (if known)    */
  float    distanceErrorM;       /* |estimated - true|, per-link ranging error*/
  uint32_t lastSeenMs;
} BleNeighbor;

typedef struct {
  bool     valid;                /* a position was actually solved            */
  uint8_t  anchorsUsed;
  float    estX, estY;
  float    trueX, trueY;
  float    errorM;               /* this round                                */
  float    meanErrorM;           /* running mean over all valid rounds        */
  float    rmseM;                /* running RMSE over all valid rounds        */
  uint32_t rounds;               /* valid solutions so far                    */
  uint8_t  nearestId;            /* strongest neighbour (proximity result)    */
  int8_t   nearestRssi;
  float    nearestDistM;
} LocalizationResult;

void bleBegin(void);                 /* start the stack and advertise         */
void bleEnd(void);                   /* deinit and free the controller RAM    */
bool bleIsActive(void);

/* Run one complete localization round: scan, filter, range, solve, score.
 * Blocks for about BLE_SCAN_MS.  Returns true if a position was solved.      */
bool bleRunRound(void);

const BleNeighbor        *bleNeighbors(void);     /* indexed by node id       */
const LocalizationResult *bleResult(void);

/* Pack the result for transmission to the sink.                              */
void  bleFillLocPayload(LocPayload *p);

/* Calibration helpers                                                        */
void  bleCalibrate1m(uint8_t nodeId);   /* record RSSI@1m against one node    */
float bleGetRssiAt1m(void);
void  bleSetPathLossN(float n);
float bleGetPathLossN(void);

/* RSSI -> metres, using the current calibration.                             */
float bleRssiToDistance(float rssi);

void  blePrintStatus(void);             /* for the `loc` serial command       */

#endif /* WSN_BLE_H */
