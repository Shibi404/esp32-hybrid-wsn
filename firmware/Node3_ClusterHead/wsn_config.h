/* =============================================================================
 *  wsn_config.h  --  Network-wide configuration for the ESP32 Hybrid WSN
 *
 *  THIS IS THE ONLY FILE YOU NORMALLY NEED TO EDIT.
 *
 *  Everything that depends on your physical setup lives here or in the matching
 *  wsn_config.cpp:  board MAC addresses, anchor coordinates, GPIO pins, radio
 *  channel, timing constants and protocol tuning weights.
 *
 *  23CSE341 Wireless Sensor Networks -- Lab Evaluation 2
 * ========================================================================== */

#ifndef WSN_CONFIG_H
#define WSN_CONFIG_H

#include <Arduino.h>

/* -----------------------------------------------------------------------------
 * 1.  NODE IDENTITY
 *
 * NODE_ID and NODE_ROLE are *runtime* globals, defined by each sketch's .ino
 * file.  They are deliberately not compile-time constants: Arduino compiles
 * every .cpp in the sketch folder as a separate translation unit, so a #define
 * placed in the .ino would not reach these shared modules.  Keeping them as
 * externs means the shared .cpp files are byte-identical across all four nodes.
 * -------------------------------------------------------------------------- */

#define NUM_NODES         4
#define NODE_N1           1       /* Sensor node            -- DHT22          */
#define NODE_N2           2       /* Sensor / relay node    -- MQ2            */
#define NODE_N3           3       /* Cluster head / router  -- MQ135          */
#define NODE_N4           4       /* Sink / gateway         -- HC-SR501 PIR   */

#define SINK_ID           NODE_N4
#define CLUSTER_HEAD_ID   NODE_N3
#define BROADCAST_ID      0xFF

typedef enum : uint8_t {
  ROLE_SENSOR       = 1,          /* originates data only                     */
  ROLE_SENSOR_RELAY = 2,          /* originates data and forwards for others  */
  ROLE_CLUSTER_HEAD = 3,          /* schedules, aggregates, routes            */
  ROLE_SINK         = 4           /* collects, measures, hosts the dashboard  */
} NodeRole;

extern uint8_t  NODE_ID;          /* defined in each NodeX.ino                */
extern NodeRole NODE_ROLE;        /* defined in each NodeX.ino                */

/* MAC address table.  Index 0 is unused so that NODE_MAC[n] matches node n.
 * Fill this in from tools/print_mac -- see wsn_config.cpp.                   */
extern const uint8_t NODE_MAC[NUM_NODES + 1][6];
extern const uint8_t BROADCAST_MAC[6];

/* Human-readable names, indexed by node id (index 0 = "??").                 */
extern const char *NODE_NAME[NUM_NODES + 1];

/* Returns the node id owning a MAC address, or 0 if unknown.                 */
uint8_t  wsnNodeIdFromMac(const uint8_t *mac);
const uint8_t *wsnMacForNode(uint8_t id);
const char *wsnRoleName(NodeRole r);

/* -----------------------------------------------------------------------------
 * 2.  PHYSICAL LAYOUT  (BLE localization)
 *
 * Positions in metres, in a local 2-D frame you define with a tape measure.
 * N2, N3 and N4 are ANCHORS -- their coordinates must be accurate, because the
 * trilateration solution is only as good as the anchor geometry.
 * N1 is the MOBILE node; NODE_POS[1] holds its *ground truth* position, used
 * solely to compute the localization error.
 * -------------------------------------------------------------------------- */

typedef struct { float x, y; } Point2;

extern Point2 NODE_POS[NUM_NODES + 1];   /* mutable: `settrue` can update N1  */
extern bool   NODE_IS_ANCHOR[NUM_NODES + 1];

/* -----------------------------------------------------------------------------
 * 3.  GPIO PIN MAP
 *
 * One sensor per board.  CRITICAL: both MQ analog outputs must sit on ADC1
 * (GPIO 32-39).  ADC2 is owned by the Wi-Fi driver and cannot be read while
 * Wi-Fi is running -- an MQ sensor on an ADC2 pin returns garbage, silently.
 *
 * Both MQ modules are powered from 3V3 (NOT 5V), so their analog output can
 * never exceed 3.3 V and no divider is fitted.  See docs/WIRING.md.
 * -------------------------------------------------------------------------- */

#define PIN_DHT22           4      /* N1: DHT22 DATA, 10k pull-up to 3V3      */
#define PIN_MQ2_AOUT        34     /* N2: MQ2 AOUT   -- ADC1_CH6, input-only  */
#define PIN_MQ2_DOUT        18     /* N2: MQ2 DOUT   -- optional cross-check  */
#define PIN_MQ135_AOUT      35     /* N3: MQ135 AOUT -- ADC1_CH7, input-only  */
#define PIN_MQ135_DOUT      18     /* N3: MQ135 DOUT -- optional cross-check  */
#define PIN_PIR             27     /* N4: HC-SR501 OUT, 3.3 V TTL             */

#define PIN_LED             2      /* on-board LED: slot / sleep indication   */
#define PIN_LED_EXT         5      /* optional external LED (N3 slot activity)*/

/* -----------------------------------------------------------------------------
 * 4.  SENSOR CONDITIONING
 * -------------------------------------------------------------------------- */

#define MQ_SUPPLY_V         3.3f   /* MQ modules are on the 3V3 rail          */
#define MQ_DIVIDER_GAIN     1.0f   /* 1.0 = direct connection (no divider).
                                    * Set to 2.0 if you later move the MQ
                                    * sensors to 5 V behind a 10k/10k divider. */
#define MQ_RL_KOHM          10.0f  /* load resistor on the breakout board     */
#define MQ_ADC_SAMPLES      16     /* multisampling to fight ADC noise        */

#define MQ2_CLEAN_AIR_RATIO   9.83f  /* MQ-2 datasheet Rs/R0 in clean air     */
#define MQ135_CLEAN_AIR_RATIO 3.60f  /* MQ-135 datasheet Rs/R0 in clean air   */

/* Curve-fit coefficients: ppm = a * (Rs/R0)^b  (datasheet log-log fits)      */
#define MQ2_SMOKE_A      3616.1f
#define MQ2_SMOKE_B      (-2.675f)
#define MQ135_CO2_A       110.47f
#define MQ135_CO2_B      (-2.862f)

#define SENSOR_WARMUP_MS      120000UL  /* MQ pre-heat per power-up (2 min)   */
#define DHT_MIN_INTERVAL_MS     2200UL  /* DHT22 minimum sampling period      */
#define SENSOR_SAMPLE_MS        1000UL  /* sensor task period                 */
#define SENSOR_MAX_FAILS             3  /* consecutive fails -> mark invalid  */

/* Alarm thresholds -- these drive the DYNAMIC SCHEDULE UPDATE demonstration. */
#define ALARM_SMOKE_PPM      800.0f
#define ALARM_AIRQ_PPM      1000.0f
#define ALARM_TEMP_C          40.0f

/* -----------------------------------------------------------------------------
 * 5.  RADIO / ESP-NOW
 * -------------------------------------------------------------------------- */

#define WSN_WIFI_CHANNEL      1    /* ESP-NOW and the sink SoftAP share ch 1  */
#define SOFTAP_SSID           "WSN_SINK"
#define SOFTAP_PASS           "wsnlab2024"

/* -----------------------------------------------------------------------------
 * 6.  MAC LAYER  (CSMA/CA)
 * -------------------------------------------------------------------------- */

#define ENABLE_PHY_CCA        1    /* 1 = promiscuous-mode physical carrier
                                    *     sense.  Set to 0 to fall back to
                                    *     virtual (NAV) sensing only if the
                                    *     lab RF environment is saturated.    */

#define CCA_RSSI_THRESHOLD  (-75)  /* dBm: above this the medium counts busy  */
#define CCA_BUSY_WINDOW_US   3000  /* medium is busy for this long after a hit*/
#define MAC_SLOT_US           500  /* one backoff slot                        */
#define MAC_DIFS_US          2000  /* idle time required before backoff       */
#define MAC_SIFS_US           300  /* gap before an ACK is returned           */
#define MAC_CW_MIN              8  /* initial contention window, in slots     */
#define MAC_CW_MAX             64  /* ceiling after exponential doubling      */
#define MAC_ACK_TIMEOUT_MS     60  /* how long to wait for an application ACK */
#define MAC_MAX_RETRIES         4  /* then drop and penalise the link         */
#define MAC_SENSE_TIMEOUT_MS   50  /* if busy this long, transmit anyway and
                                    * count it as a forced transmission       */
#define MAC_TXQ_DEPTH           6  /* outbound queue depth                    */

/* -----------------------------------------------------------------------------
 * 7.  TDMA SUPERFRAME
 *
 * 8 slots x 250 ms = 2000 ms superframe.  Slot 3 is the dynamic slot: it is
 * unassigned (and therefore a sleep slot) until a node raises an alarm or
 * reports a backlog, at which point the cluster head grants it.
 * -------------------------------------------------------------------------- */

#define TDMA_NUM_SLOTS          8
#define TDMA_SLOT_MS          250UL
#define TDMA_SUPERFRAME_MS    (TDMA_NUM_SLOTS * TDMA_SLOT_MS)
#define TDMA_GUARD_MS          20UL  /* guard band at each slot edge          */

#define SLOT_BEACON             0    /* N3 broadcasts schedule + time sync    */
#define SLOT_N1_DATA            1
#define SLOT_N2_DATA            2
#define SLOT_DYNAMIC            3    /* granted on demand, else sleep         */
#define SLOT_AGG_UPLINK         4    /* N3 -> N4 aggregated data              */
#define SLOT_CAP                5    /* contention access period              */
#define SLOT_SINK_STATUS        6    /* N4 broadcasts network status          */
#define SLOT_SLEEP              7    /* everybody idle                        */

#define SCHED_SILENT_LIMIT      3    /* missed slots before a slot is revoked */
#define SCHED_BACKLOG_LIMIT     3    /* queued frames that justify a grant    */

#define ENABLE_LIGHT_SLEEP      0    /* 0 = radio-off sleep (esp_wifi_stop),
                                      *     CPU stays alive so serial logging
                                      *     keeps running -- best for a demo.
                                      * 1 = true esp_light_sleep_start(), the
                                      *     whole SoC sleeps.  More impressive
                                      *     but serial goes quiet and TDMA
                                      *     timing is tighter.                */

/* -----------------------------------------------------------------------------
 * 8.  BLE LOCALIZATION
 * -------------------------------------------------------------------------- */

#define BLE_PHASE_MS        6000UL   /* duration of each BLE phase            */
#define BLE_SCAN_MS         4000UL   /* active scan window within the phase   */
#define BLE_SCAN_INTERVAL    100     /* in 0.625 ms units -> 62.5 ms          */
#define BLE_SCAN_WINDOW       80     /* in 0.625 ms units -> 50 ms            */
#define BLE_ADV_MIN          0x20    /* in 0.625 ms units -> 20 ms            */
#define BLE_ADV_MAX          0x40    /* in 0.625 ms units -> 40 ms            */
#define BLE_NAME_PREFIX      "WSN-N" /* advertised as WSN-N1 ... WSN-N4       */

#define BLE_RSSI_SAMPLES_MAX   16    /* per-neighbour sample buffer           */
#define BLE_RSSI_MIN_SAMPLES    3    /* fewer than this -> neighbour ignored  */
#define BLE_RSSI_EWMA_ALPHA   0.30f  /* smoothing across localization rounds  */

#define BLE_PATH_LOSS_N       2.50f  /* indoor path-loss exponent             */
#define BLE_RSSI_AT_1M     (-59.0f)  /* default; override with `calib1m`      */
#define BLE_MAX_RANGE_M       25.0f  /* sanity clamp on estimated distance    */

#define WIFI_PHASE_MS       30000UL  /* Wi-Fi phase length (15 superframes)   */

#define SINK_BLE_ANCHOR          1   /* The sink must advertise, otherwise N1
                                      * can only hear two anchors and 2-D
                                      * trilateration is impossible.  Set to 0
                                      * only if the sink runs out of RAM with
                                      * BLE + SoftAP + web server together --
                                      * and then localization degrades to
                                      * proximity only.                       */

/* -----------------------------------------------------------------------------
 * 9.  ROUTING
 *
 * cost = W_HOP*(hops+1)
 *      + W_RSSI*(1 - rssiNorm)      link quality
 *      + W_ACK *(1 - ackRate)       successful ACK rate
 *      + W_LOSS* lossRate           packet loss
 *      = infinity if the link is down                 (link availability)
 *
 * All five route-selection parameters named in the lab guidelines are used.
 * -------------------------------------------------------------------------- */

#define ROUTE_W_HOP           100
#define ROUTE_W_RSSI          150
#define ROUTE_W_ACK           200
#define ROUTE_W_LOSS          100
#define ROUTE_COST_INF      65535

/* Cluster-head preference, subtracted from the cost of routing via the
 * cluster head.  A sensor node should report to its head rather than straight
 * to the sink, because that is what makes aggregation possible at all --
 * going around the head would deliver the same data in more packets.  The
 * bonus quantifies that benefit so the head still has to compete on cost:
 * once its link degrades or fails, an ordinary neighbour wins normally.
 * This is the same idea as LEACH's cluster-member behaviour.               */
#define ROUTE_W_CH_BONUS      150

/* -----------------------------------------------------------------------------
 *  INTENDED TOPOLOGY
 *
 *  On a lab bench all four boards sit within radio range of each other, so
 *  every node could reach the sink in one hop and the multi-hop, aggregation
 *  and route-recovery mechanisms would never actually run.  This matrix
 *  declares the topology the deployment is meant to have:
 *
 *        N1 ──── N2 ──── N4            N1 has NO direct link to the sink
 *         \      /       /
 *          \    /       /
 *           N3 ────────'
 *
 *  It is a routing-policy constraint and it is stated openly -- not a claim
 *  about what the radios can hear.  Spreading the nodes down a corridor would
 *  produce the same topology physically; set every entry to 1 if you do that.
 * -------------------------------------------------------------------------- */
extern const uint8_t ROUTE_LINK_ALLOWED[NUM_NODES + 1][NUM_NODES + 1];
bool routeLinkAllowed(uint8_t a, uint8_t b);

#define RSSI_NORM_MIN       (-95)    /* maps to rssiNorm = 0                  */
#define RSSI_NORM_MAX       (-35)    /* maps to rssiNorm = 1                  */

#define ROUTE_FAIL_LIMIT        3    /* consecutive ACK losses -> link down   */
#define NEIGHBOR_TIMEOUT_MS  7000UL  /* silence this long -> link down        */
#define HELLO_INTERVAL_MS    2000UL  /* one HELLO per superframe              */
#define WSN_DEFAULT_TTL         5

/* -----------------------------------------------------------------------------
 * 10.  MISC
 * -------------------------------------------------------------------------- */

#define WSN_FW_VERSION      "1.0.0"
#define NVS_NAMESPACE       "wsn"
#define SERIAL_BAUD         115200

#endif /* WSN_CONFIG_H */
