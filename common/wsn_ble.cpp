/* =============================================================================
 *  wsn_ble.cpp  --  BLE advertising, scanning, RSSI ranging and trilateration.
 * ========================================================================== */

#include "wsn_ble.h"
#include "wsn_log.h"
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <math.h>

#define TAG "BLE"

static BleNeighbor        s_nbr[NUM_NODES + 1];
static LocalizationResult s_loc;

static BLEScan   *s_scan      = nullptr;
static bool       s_active    = false;
static float      s_rssiAt1m  = BLE_RSSI_AT_1M;
static float      s_plExp     = BLE_PATH_LOSS_N;

/* Per-round raw sample buffers, indexed by node id.                          */
static int8_t  s_buf[NUM_NODES + 1][BLE_RSSI_SAMPLES_MAX];
static uint8_t s_bufN[NUM_NODES + 1];

/* Running accumulators for mean / RMSE of the localization error.            */
static double  s_errSum   = 0.0;
static double  s_errSqSum = 0.0;

/* -------------------------------------------------------------------------- */

static void resetRound(void) {
  for (uint8_t i = 0; i <= NUM_NODES; i++) {
    s_bufN[i]       = 0;
    s_nbr[i].seen    = false;
    s_nbr[i].samples = 0;
  }
}

/* Map "WSN-N3" -> 3.  Returns 0 if the name is not one of ours.              */
static uint8_t nodeIdFromAdvName(const String &name) {
  const size_t pfx = strlen(BLE_NAME_PREFIX);
  if (name.length() != pfx + 1) return 0;
  if (!name.startsWith(BLE_NAME_PREFIX)) return 0;
  const char c = name.charAt(pfx);
  if (c < '1' || c > ('0' + NUM_NODES)) return 0;
  return (uint8_t)(c - '0');
}

static void recordSample(uint8_t id, int8_t rssi) {
  if (id < 1 || id > NUM_NODES || id == NODE_ID) return;
  if (s_bufN[id] < BLE_RSSI_SAMPLES_MAX) s_buf[id][s_bufN[id]++] = rssi;
  s_nbr[id].seen       = true;
  s_nbr[id].rssiRaw    = rssi;
  s_nbr[id].lastSeenMs = millis();
}

/* --- scan callback (signature differs between core 2.x and 3.x) ----------- */

class WsnScanCb : public BLEAdvertisedDeviceCallbacks {
  void handle(BLEAdvertisedDevice &dev) {
    if (!dev.haveName()) return;
    /* std::string (core 2.x) and String (core 3.x) both expose c_str(), so
     * routing through it keeps this one line portable across both cores.    */
    const String name = String(dev.getName().c_str());
    const uint8_t id  = nodeIdFromAdvName(name);
    if (id) recordSample(id, (int8_t)dev.getRSSI());
  }
public:
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void onResult(BLEAdvertisedDevice *dev) override { if (dev) handle(*dev); }
#else
  void onResult(BLEAdvertisedDevice dev) override { handle(dev); }
#endif
};

static WsnScanCb s_scanCb;

/* -------------------------------------------------------------------------- */

/* Restore any previously stored ranging calibration.  Done once, lazily, so a
 * board that was calibrated last week still ranges correctly today.          */
static void loadStoredCalibration(void) {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;

  Preferences nvs;
  nvs.begin(NVS_NAMESPACE, true);
  const float r1 = nvs.getFloat("rssi1m", 0.0f);
  const float pl = nvs.getFloat("plexp",  0.0f);
  nvs.end();

  if (r1 < -20.0f && r1 > -100.0f) {
    s_rssiAt1m = r1;
    LOGI(TAG, "Loaded stored RSSI@1m = %.1f dBm", s_rssiAt1m);
  }
  if (pl >= 1.5f && pl <= 4.5f) {
    s_plExp = pl;
    LOGI(TAG, "Loaded stored path-loss exponent = %.2f", s_plExp);
  }
}

void bleBegin(void) {
  if (s_active) return;
  loadStoredCalibration();

  char devName[16];
  snprintf(devName, sizeof(devName), "%s%u", BLE_NAME_PREFIX, NODE_ID);

  BLEDevice::init(devName);

  /* --- advertising: make this node discoverable by every other node ------ */
  BLEAdvertising *adv = BLEDevice::getAdvertising();

  BLEAdvertisementData advData;
  advData.setFlags(0x06);          /* LE General Discoverable, BR/EDR not supported */
  advData.setName(devName);
  adv->setAdvertisementData(advData);

  /* Repeat the name in the scan response as well.  An active scanner then
   * gets it even if the primary advertisement is truncated by the stack.   */
  BLEAdvertisementData scanRsp;
  scanRsp.setName(devName);
  adv->setScanResponseData(scanRsp);

  adv->setMinInterval(BLE_ADV_MIN);
  adv->setMaxInterval(BLE_ADV_MAX);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  /* --- scanning: discover the neighbours ---------------------------------- */
  s_scan = BLEDevice::getScan();
  s_scan->setAdvertisedDeviceCallbacks(&s_scanCb, /*wantDuplicates=*/true);
  s_scan->setActiveScan(true);       /* request scan responses                */
  s_scan->setInterval(BLE_SCAN_INTERVAL);
  s_scan->setWindow(BLE_SCAN_WINDOW);

  /* Duplicates must be reported, otherwise the controller filters out every
   * advertisement after the first and we end up with one RSSI sample per
   * neighbour per round -- far too few to median-filter.                    */

  s_active = true;
  LOGI(TAG, "BLE up: advertising as \"%s\", active scan %d/%d (interval/window, "
            "x0.625 ms)", devName, BLE_SCAN_INTERVAL, BLE_SCAN_WINDOW);
}

void bleEnd(void) {
  if (!s_active) return;
  if (s_scan) { s_scan->stop(); s_scan->clearResults(); }
  BLEDevice::stopAdvertising();
  /* release_memory = true hands the BT controller's RAM back to the heap.
   * This is what makes it possible for the sink to run BLE, ESP-NOW, a SoftAP
   * and a web server on one chip.                                           */
  BLEDevice::deinit(true);
  s_scan   = nullptr;
  s_active = false;
  LOGI(TAG, "BLE down, controller memory released (free heap %lu B)",
       (unsigned long)ESP.getFreeHeap());
}

bool bleIsActive(void) { return s_active; }

/* -----------------------------------------------------------------------------
 *  Filtering
 * -------------------------------------------------------------------------- */

static int8_t medianOf(int8_t *a, uint8_t n) {
  /* insertion sort -- n <= 16, so anything cleverer would be noise           */
  for (uint8_t i = 1; i < n; i++) {
    const int8_t key = a[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
  return (n & 1) ? a[n / 2] : (int8_t)((a[n / 2 - 1] + a[n / 2]) / 2);
}

float bleRssiToDistance(float rssi) {
  /* Log-distance path loss:  RSSI(d) = RSSI(1m) - 10 * n * log10(d)
   *                 =>      d = 10 ^ ((RSSI(1m) - RSSI) / (10 * n))         */
  const float d = powf(10.0f, (s_rssiAt1m - rssi) / (10.0f * s_plExp));
  if (!isfinite(d) || d < 0.05f) return 0.05f;
  if (d > BLE_MAX_RANGE_M)       return BLE_MAX_RANGE_M;
  return d;
}

/* -----------------------------------------------------------------------------
 *  Least-squares trilateration.
 *
 *  Each anchor i gives a circle  (x-xi)^2 + (y-yi)^2 = di^2.  Subtracting the
 *  reference anchor's equation from the others cancels the quadratic terms and
 *  leaves a linear system A*p = b, which we solve in the least-squares sense
 *  via the 2x2 normal equations.  With 3 anchors the system is exactly
 *  determined; with more it is overdetermined and the least-squares solution
 *  averages out ranging noise.
 * -------------------------------------------------------------------------- */
static bool trilaterate(const uint8_t *ids, const float *d, uint8_t n,
                        float *outX, float *outY) {
  if (n < 3) return false;

  const uint8_t ref = ids[n - 1];
  const float xr = NODE_POS[ref].x, yr = NODE_POS[ref].y, dr = d[n - 1];

  double Saa = 0, Sab = 0, Sbb = 0, Sac = 0, Sbc = 0;

  for (uint8_t i = 0; i < n - 1; i++) {
    const uint8_t id = ids[i];
    const float xi = NODE_POS[id].x, yi = NODE_POS[id].y, di = d[i];

    const double a = 2.0 * (xr - xi);
    const double b = 2.0 * (yr - yi);
    const double c = (double)di * di - (double)dr * dr
                   - (double)xi * xi - (double)yi * yi
                   + (double)xr * xr + (double)yr * yr;

    Saa += a * a;  Sab += a * b;  Sbb += b * b;
    Sac += a * c;  Sbc += b * c;
  }

  const double det = Saa * Sbb - Sab * Sab;
  /* A near-zero determinant means the anchors are collinear -- geometrically
   * they cannot fix a 2-D position.  Say so rather than returning a wild
   * extrapolation.                                                          */
  if (fabs(det) < 1e-9) {
    LOGW(TAG, "Trilateration singular: anchors are (nearly) collinear. "
              "Move one anchor off the line.");
    return false;
  }

  *outX = (float)((Sbb * Sac - Sab * Sbc) / det);
  *outY = (float)((Saa * Sbc - Sab * Sac) / det);
  return isfinite(*outX) && isfinite(*outY);
}

/* -------------------------------------------------------------------------- */

bool bleRunRound(void) {
  if (!s_active) { LOGW(TAG, "bleRunRound() called while BLE is down"); return false; }

  resetRound();

  const uint32_t secs = (BLE_SCAN_MS + 999) / 1000;
  LOGI(TAG, "Scanning for %lu s ...", (unsigned long)secs);
  s_scan->start((uint32_t)secs, false);
  s_scan->clearResults();

  /* --- per-neighbour filtering and ranging -------------------------------- */
  uint8_t found = 0;
  s_loc.nearestId   = 0;
  s_loc.nearestRssi = -127;

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    BleNeighbor &n = s_nbr[i];
    if (i == NODE_ID || s_bufN[i] < BLE_RSSI_MIN_SAMPLES) {
      if (i != NODE_ID && s_bufN[i] > 0)
        LOGW(TAG, "N%u: only %u samples (need %d) - ignored this round",
             i, s_bufN[i], BLE_RSSI_MIN_SAMPLES);
      n.seen = false;
      continue;
    }

    n.samples    = s_bufN[i];
    n.rssiMedian = medianOf(s_buf[i], s_bufN[i]);

    /* EWMA across rounds; seed it on the first sighting so the filter does
     * not spend several rounds crawling up from zero.                        */
    n.rssiFiltered = (n.rssiFiltered == 0.0f)
                   ? (float)n.rssiMedian
                   : (BLE_RSSI_EWMA_ALPHA * n.rssiMedian +
                      (1.0f - BLE_RSSI_EWMA_ALPHA) * n.rssiFiltered);

    n.distanceM = bleRssiToDistance(n.rssiFiltered);

    /* If both ends have known coordinates we can also score the *ranging*
     * error on its own, independently of the position solution.             */
    if (NODE_IS_ANCHOR[i] && (NODE_IS_ANCHOR[NODE_ID] || NODE_ID == NODE_N1)) {
      const float dx = NODE_POS[i].x - NODE_POS[NODE_ID].x;
      const float dy = NODE_POS[i].y - NODE_POS[NODE_ID].y;
      n.trueDistanceM  = sqrtf(dx * dx + dy * dy);
      n.distanceErrorM = fabsf(n.distanceM - n.trueDistanceM);
    } else {
      n.trueDistanceM  = NAN;
      n.distanceErrorM = NAN;
    }

    found++;
    if (n.rssiMedian > s_loc.nearestRssi) {
      s_loc.nearestId    = i;
      s_loc.nearestRssi  = n.rssiMedian;
      s_loc.nearestDistM = n.distanceM;
    }

    LOGI(TAG, "N%u: RSSI med=%d dBm (n=%u, ewma=%.1f)  d_est=%.2f m%s",
         i, n.rssiMedian, n.samples, n.rssiFiltered, n.distanceM,
         isfinite(n.trueDistanceM)
           ? String(String("  d_true=") + String(n.trueDistanceM, 2) +
                    " m  err=" + String(n.distanceErrorM, 2) + " m").c_str()
           : "");
    logCsv("BLE,%u,%d,%u,%.2f,%.2f", i, n.rssiMedian, n.samples,
           n.distanceM, isfinite(n.distanceErrorM) ? n.distanceErrorM : -1.0f);
  }

  if (found == 0) {
    LOGW(TAG, "No WSN neighbours discovered. Are the other nodes powered and "
              "in their BLE phase?");
    s_loc.valid = false;
    return false;
  }

  LOGI(TAG, "Nearest node: N%u at %d dBm (~%.2f m)  [proximity result]",
       s_loc.nearestId, s_loc.nearestRssi, s_loc.nearestDistM);

  /* --- position solution -------------------------------------------------- */
  uint8_t ids[NUM_NODES];
  float   dists[NUM_NODES];
  uint8_t na = 0;

  for (uint8_t i = 1; i <= NUM_NODES && na < NUM_NODES; i++) {
    if (i == NODE_ID || !s_nbr[i].seen || !NODE_IS_ANCHOR[i]) continue;
    ids[na]   = i;
    dists[na] = s_nbr[i].distanceM;
    na++;
  }

  s_loc.anchorsUsed = na;
  s_loc.trueX = NODE_POS[NODE_ID].x;
  s_loc.trueY = NODE_POS[NODE_ID].y;

  if (na < 3) {
    LOGW(TAG, "Localization needs 3 anchors, heard %u. Reporting proximity "
              "only this round.", na);
    s_loc.valid = false;
    return false;
  }

  float ex, ey;
  if (!trilaterate(ids, dists, na, &ex, &ey)) { s_loc.valid = false; return false; }

  s_loc.estX = ex;
  s_loc.estY = ey;

  const float dx = ex - s_loc.trueX, dy = ey - s_loc.trueY;
  s_loc.errorM = sqrtf(dx * dx + dy * dy);
  s_loc.valid  = true;
  s_loc.rounds++;

  s_errSum   += s_loc.errorM;
  s_errSqSum += (double)s_loc.errorM * s_loc.errorM;
  s_loc.meanErrorM = (float)(s_errSum / s_loc.rounds);
  s_loc.rmseM      = (float)sqrt(s_errSqSum / s_loc.rounds);

  LOGI(TAG, "LOCALIZATION: estimated (%.2f, %.2f) m | true (%.2f, %.2f) m | "
            "error %.2f m | mean %.2f m | RMSE %.2f m | %u anchors, round %lu",
       s_loc.estX, s_loc.estY, s_loc.trueX, s_loc.trueY, s_loc.errorM,
       s_loc.meanErrorM, s_loc.rmseM, na, (unsigned long)s_loc.rounds);
  logCsv("LOC,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u",
         s_loc.estX, s_loc.estY, s_loc.trueX, s_loc.trueY,
         s_loc.errorM, s_loc.rmseM, na);

  return true;
}

/* -------------------------------------------------------------------------- */

const BleNeighbor        *bleNeighbors(void) { return s_nbr; }
const LocalizationResult *bleResult(void)    { return &s_loc; }

void bleFillLocPayload(LocPayload *p) {
  memset(p, 0, sizeof(*p));
  p->nodeId      = NODE_ID;
  p->nAnchors    = s_loc.anchorsUsed;
  p->nearestId   = s_loc.nearestId;
  p->nearestRssi = s_loc.nearestRssi;
  p->estX        = s_loc.valid ? s_loc.estX : NAN;
  p->estY        = s_loc.valid ? s_loc.estY : NAN;
  p->trueX       = s_loc.trueX;
  p->trueY       = s_loc.trueY;
  p->errorM      = s_loc.valid ? s_loc.errorM : NAN;
  p->rmseM       = s_loc.rounds ? s_loc.rmseM : NAN;

  for (uint8_t i = 0; i <= NUM_NODES; i++) {
    p->rssi[i]   = s_nbr[i].seen ? s_nbr[i].rssiMedian : 0;
    const float d = s_nbr[i].seen ? s_nbr[i].distanceM : 0.0f;
    p->distCm[i] = (uint16_t)constrain((int)(d * 100.0f), 0, 65535);
  }
}

/* -------------------------------------------------------------------------- */

void bleCalibrate1m(uint8_t nodeId) {
  if (nodeId < 1 || nodeId > NUM_NODES || nodeId == NODE_ID) {
    LOGE(TAG, "calib1m: give a valid neighbour node id (1..%d)", NUM_NODES);
    return;
  }
  if (!s_active) { LOGE(TAG, "calib1m: run it during the BLE phase"); return; }

  LOGI(TAG, "Place this node EXACTLY 1.0 m from N%u, then hold still...", nodeId);
  delay(2000);

  resetRound();
  s_scan->start(4, false);
  s_scan->clearResults();

  if (s_bufN[nodeId] < BLE_RSSI_MIN_SAMPLES) {
    LOGE(TAG, "calib1m: only %u samples from N%u - move closer or retry",
         s_bufN[nodeId], nodeId);
    return;
  }

  const int8_t med = medianOf(s_buf[nodeId], s_bufN[nodeId]);
  s_rssiAt1m = (float)med;

  Preferences nvs;
  nvs.begin(NVS_NAMESPACE, false);
  nvs.putFloat("rssi1m", s_rssiAt1m);
  nvs.end();

  LOGI(TAG, "Calibrated RSSI@1m = %.1f dBm from %u samples (saved to NVS)",
       s_rssiAt1m, s_bufN[nodeId]);
}

float bleGetRssiAt1m(void) { return s_rssiAt1m; }
void  bleSetPathLossN(float n) {
  if (n >= 1.5f && n <= 4.5f) {
    s_plExp = n;
    Preferences nvs;
    nvs.begin(NVS_NAMESPACE, false);
    nvs.putFloat("plexp", n);
    nvs.end();
    LOGI(TAG, "Path-loss exponent set to %.2f (saved)", n);
  } else {
    LOGE(TAG, "Path-loss exponent must be between 1.5 and 4.5 (indoor ~2.0-3.0)");
  }
}
float bleGetPathLossN(void) { return s_plExp; }

void blePrintStatus(void) {
  Serial.println();
  Serial.printf("  BLE state     : %s\r\n", s_active ? "active" : "down");
  Serial.printf("  RSSI @ 1 m    : %.1f dBm   path-loss n = %.2f\r\n",
                s_rssiAt1m, s_plExp);
  Serial.println("  --- neighbours ---------------------------------------------");
  Serial.println("   node   RSSI(med)  samples   d_est(m)   d_true(m)   err(m)");
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i == NODE_ID) continue;
    const BleNeighbor &n = s_nbr[i];
    if (!n.seen) { Serial.printf("   N%-5u  --        --        --         --          --\r\n", i); continue; }
    Serial.printf("   N%-5u  %-9d %-9u %-10.2f %-11s %s\r\n",
                  i, n.rssiMedian, n.samples, n.distanceM,
                  isfinite(n.trueDistanceM)  ? String(n.trueDistanceM, 2).c_str()  : "--",
                  isfinite(n.distanceErrorM) ? String(n.distanceErrorM, 2).c_str() : "--");
  }
  Serial.println("  --- localization -------------------------------------------");
  if (s_loc.nearestId)
    Serial.printf("   Nearest node : N%u (%d dBm, ~%.2f m)\r\n",
                  s_loc.nearestId, s_loc.nearestRssi, s_loc.nearestDistM);
  if (s_loc.valid) {
    Serial.printf("   Estimated    : (%.2f, %.2f) m   from %u anchors\r\n",
                  s_loc.estX, s_loc.estY, s_loc.anchorsUsed);
    Serial.printf("   Ground truth : (%.2f, %.2f) m\r\n", s_loc.trueX, s_loc.trueY);
    Serial.printf("   Error        : %.2f m   mean %.2f m   RMSE %.2f m   "
                  "over %lu rounds\r\n",
                  s_loc.errorM, s_loc.meanErrorM, s_loc.rmseM,
                  (unsigned long)s_loc.rounds);
  } else {
    Serial.println("   No position solution yet (needs 3 anchors heard).");
  }
  Serial.println();
}
