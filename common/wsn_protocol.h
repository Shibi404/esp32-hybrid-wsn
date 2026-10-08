/* =============================================================================
 *  wsn_protocol.h  --  Wire format for the hybrid WSN.
 *
 *  Every frame on the air is:
 *
 *      +----------------+------------------+--------+
 *      |  WsnHeader     |  payload (0..N)  | CRC-16 |
 *      |  21 bytes      |  type dependent  | 2 B    |
 *      +----------------+------------------+--------+
 *
 *  Frames are packed little-endian structs, serialised by wsnSerialize() and
 *  validated by wsnParse().  ESP-NOW allows 250 payload bytes; our largest
 *  frame (aggregated data) is 75 bytes, leaving plenty of headroom.
 *
 *  Design notes
 *  ------------
 *  * `dst` is the NEXT HOP, `finalDst` is the ultimate destination.  Keeping
 *    them separate is what makes multi-hop forwarding possible.
 *  * `path[]` records the nodes traversed.  It gives the sink a printable
 *    route string ("1->3->4") and lets forwarders drop looping frames.
 *  * `durationUs` is the Network Allocation Vector (NAV) value: how long the
 *    sender expects to occupy the medium.  Any node that overhears the frame
 *    treats the channel as busy for that long -- this is virtual carrier
 *    sense, the same idea as 802.11 RTS/CTS duration fields.
 *  * `txTimeMs` is stamped in *network* time (synchronised to the cluster
 *    head's beacon) so the sink can compute true end-to-end delay.
 * ========================================================================== */

#ifndef WSN_PROTOCOL_H
#define WSN_PROTOCOL_H

#include <Arduino.h>
#include "wsn_config.h"

#define WSN_MAGIC        0xA5
#define WSN_PROTO_VER    0x01
#define WSN_MAX_FRAME    250

/* --- frame types --------------------------------------------------------- */
typedef enum : uint8_t {
  FT_HELLO        = 1,   /* BROADCAST  neighbour discovery / liveness / hops  */
  FT_BEACON       = 2,   /* BROADCAST  TDMA schedule + time sync              */
  FT_DATA         = 3,   /* UNICAST    raw sensor reading                     */
  FT_AGG          = 4,   /* UNICAST    aggregated reading from cluster head   */
  FT_ACK          = 5,   /* UNICAST    MAC-layer acknowledgement              */
  FT_ROUTE_UPDATE = 6,   /* BROADCAST  link state change / route control      */
  FT_NET_STATUS   = 7,   /* BROADCAST  sink -> network performance summary    */
  FT_CMD          = 8,   /* BROADCAST  demonstration control command          */
  FT_SLOT_REQ     = 9,   /* UNICAST    slot request / backlog report to CH    */
  FT_LOC          = 10   /* UNICAST    BLE localization result -> sink        */
} FrameType;

const char *wsnFrameTypeName(uint8_t t);

/* --- header -------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
  uint8_t  magic;        /* WSN_MAGIC -- cheap pre-CRC sanity check           */
  uint8_t  ver;          /* WSN_PROTO_VER                                     */
  uint8_t  type;         /* FrameType                                         */
  uint8_t  src;          /* ORIGINATOR node id (never rewritten on a hop)     */
  uint8_t  dst;          /* NEXT HOP node id, or BROADCAST_ID                 */
  uint8_t  finalDst;     /* ultimate destination node id                      */
  uint8_t  hop;          /* hops already traversed                            */
  uint8_t  ttl;          /* decremented per hop; 0 -> drop                    */
  uint16_t seq;          /* per-originator sequence number                    */
  uint32_t txTimeMs;     /* NETWORK time at origination (end-to-end delay)    */
  uint16_t durationUs;   /* NAV: expected medium occupancy                    */
  uint8_t  pathLen;      /* number of valid entries in path[]                 */
  uint8_t  path[4];      /* node ids traversed, oldest first                  */
} WsnHeader;             /* = 21 bytes                                        */

#define WSN_HDR_LEN   ((uint8_t)sizeof(WsnHeader))

/* --- payloads ------------------------------------------------------------ */

/* sensor validity bits (DataPayload.sensorMask / AggPayload.sensorMask)      */
#define SENS_TEMP      0x01
#define SENS_HUM       0x02
#define SENS_GAS       0x04
#define SENS_MOTION    0x08

/* alarm bits (DataPayload.alarm / AggPayload.alarmOr)                        */
#define ALARM_GAS      0x01
#define ALARM_MOTION   0x02
#define ALARM_TEMP     0x04

/* DataPayload.flags                                                          */
#define DFLAG_WARMING  0x01    /* MQ sensor still pre-heating                 */
#define DFLAG_RELAYED  0x02    /* frame travelled via the alternate route     */

typedef struct __attribute__((packed)) {
  float    temperature;  /* degC, NAN if unavailable                          */
  float    humidity;     /* %RH,  NAN if unavailable                          */
  float    gasPpm;       /* ppm (indexed), NAN if unavailable                 */
  float    gasRatio;     /* Rs/R0                                             */
  uint8_t  motion;       /* 0/1, 0xFF if unavailable                          */
  uint8_t  sensorMask;   /* which of the four fields above are valid          */
  uint8_t  alarm;        /* alarm bitfield -- drives dynamic scheduling       */
  uint8_t  queueLen;     /* backlog report, also drives dynamic scheduling    */
  uint16_t retxTotal;    /* cumulative MAC retransmissions at the source      */
  int8_t   nextHopRssi;  /* last observed RSSI of this node's next hop        */
  uint8_t  flags;        /* DFLAG_*                                           */
} DataPayload;           /* = 24 bytes                                        */

typedef struct __attribute__((packed)) {
  uint8_t  srcMask;      /* bitmap of contributing node ids (bit n-1 = Nn)    */
  uint8_t  nSamples;     /* readings folded into this aggregate               */
  uint8_t  alarmOr;      /* OR of all contributing alarm fields               */
  uint8_t  sensorMask;   /* OR of all contributing sensor masks               */
  float    tMin, tMax, tAvg;
  float    hMin, hMax, hAvg;
  float    gMin, gMax, gAvg;
  uint8_t  motionCount;  /* how many contributors reported motion             */
  uint8_t  retxTotal;    /* retransmissions at the aggregator                 */
  uint16_t rawPackets;   /* packets that were combined                        */
  uint16_t rawBytes;     /* bytes those packets would have cost on the air    */
  uint16_t aggBytes;     /* bytes this aggregate actually costs               */
  uint32_t oldestTxMs;   /* earliest contributor timestamp (worst-case delay) */
} AggPayload;            /* = 52 bytes                                        */

typedef struct __attribute__((packed)) {
  uint16_t ackSeq;       /* sequence number being acknowledged                */
  uint8_t  ackSrc;       /* originator of the acknowledged frame              */
  int8_t   rssi;         /* RSSI at which the acknowledged frame arrived      */
} AckPayload;            /* = 4 bytes                                         */

/* reason codes explaining why the schedule changed -- shown in the demo      */
typedef enum : uint8_t {
  SCHED_INIT          = 0,
  SCHED_ALARM_GRANT   = 1,   /* a node raised an alarm -> gets the dyn slot   */
  SCHED_ALARM_RELEASE = 2,   /* alarm cleared -> slot returned to sleep       */
  SCHED_NODE_SILENT   = 3,   /* node missed its slots -> slot revoked         */
  SCHED_NODE_REJOIN   = 4,   /* node came back -> slot restored               */
  SCHED_BACKLOG_GRANT = 5,   /* node reported a backlog -> extra slot         */
  SCHED_COLLISION_TEST= 6    /* `collide` demo -> two owners in one slot      */
} SchedReason;

const char *wsnSchedReasonName(uint8_t r);

/* slot kinds                                                                 */
typedef enum : uint8_t {
  SK_BEACON = 0, SK_DATA = 1, SK_AGG = 2, SK_CAP = 3, SK_STATUS = 4, SK_SLEEP = 5
} SlotKind;

typedef struct __attribute__((packed)) {
  uint32_t frameStartMs;                 /* network time of superframe start  */
  uint32_t netTimeMs;                    /* sender's network time at TX       */
  uint16_t schedVersion;                 /* bumped on every change            */
  uint16_t slotMs;
  uint8_t  numSlots;
  uint8_t  slotOwner[TDMA_NUM_SLOTS];    /* node id, 0 = unused/sleep         */
  uint8_t  slotKind[TDMA_NUM_SLOTS];     /* SlotKind                          */
  uint8_t  slotOwner2[TDMA_NUM_SLOTS];   /* 2nd owner -- collision demo only  */
  uint8_t  reason;                       /* SchedReason                       */
  uint8_t  reasonNode;                   /* node the reason refers to         */
  uint8_t  chId;                         /* cluster head id                   */
  uint8_t  sinkId;                       /* sink id                           */
} BeaconPayload;         /* = 42 bytes                                        */

/* route control events                                                       */
typedef enum : uint8_t {
  RE_LINK_DOWN = 1, RE_LINK_UP = 2, RE_ROUTE_CHANGED = 3
} RouteEvent;

const char *wsnRouteEventName(uint8_t e);

typedef struct __attribute__((packed)) {
  uint8_t  originator;   /* node reporting the event                          */
  uint8_t  event;        /* RouteEvent                                        */
  uint8_t  peerA;        /* the two endpoints of the affected link            */
  uint8_t  peerB;
  uint8_t  newNextHop;   /* originator's next hop after the change            */
  uint8_t  hopsToSink;   /* originator's new hop count                        */
  uint16_t cost;         /* originator's new route cost                       */
} RoutePayload;          /* = 8 bytes                                         */

typedef struct __attribute__((packed)) {
  uint32_t rxTotal;
  uint32_t expected;
  uint32_t lost;
  uint32_t retxTotal;
  uint16_t pdrX100;        /* packet delivery ratio   x100                    */
  uint16_t avgDelayMs;
  uint16_t throughputBps;
  uint16_t reductionX100;  /* data reduction percent  x100                    */
  uint8_t  aliveMask;      /* bit n-1 set if node n was heard recently        */
} NetStatusPayload;      /* = 23 bytes                                        */

/* demonstration commands, broadcast from the sink or any serial console      */
typedef enum : uint8_t {
  CMD_FAIL_LINK   = 1,   /* arg = node id to stop talking to                  */
  CMD_RECOVER_LINK= 2,
  CMD_COLLIDE     = 3,   /* arg = 0 soft (CCA on), 1 hard (CCA bypassed)      */
  CMD_CALIBRATE   = 4,
  CMD_RESET_STATS = 5,
  CMD_SET_RATE    = 6    /* arg = reports per superframe                      */
} CmdCode;

typedef struct __attribute__((packed)) {
  uint8_t  target;       /* node id, or BROADCAST_ID for everyone             */
  uint8_t  cmd;          /* CmdCode                                           */
  int32_t  arg;
} CmdPayload;            /* = 6 bytes                                         */

typedef struct __attribute__((packed)) {
  uint8_t  queueLen;     /* frames waiting at this node                       */
  uint8_t  alarm;        /* alarm bits currently asserted                     */
  uint8_t  wantSlots;    /* how many slots the node is asking for             */
  uint8_t  reserved;
} SlotReqPayload;        /* = 4 bytes                                         */

typedef struct __attribute__((packed)) {
  uint8_t  nodeId;                       /* node being localized (N1)         */
  uint8_t  nAnchors;                     /* anchors used in the solution      */
  uint8_t  nearestId;                    /* strongest neighbour               */
  int8_t   nearestRssi;
  float    estX, estY;                   /* trilateration result              */
  float    trueX, trueY;                 /* tape-measured ground truth        */
  float    errorM;                       /* this round's localization error   */
  float    rmseM;                        /* RMSE across all rounds so far     */
  int8_t   rssi[NUM_NODES + 1];          /* filtered RSSI per node            */
  uint16_t distCm[NUM_NODES + 1];        /* estimated distance per node       */
} LocPayload;            /* = 43 bytes                                        */

typedef struct __attribute__((packed)) {
  uint8_t  role;
  uint8_t  hopsToSink;   /* 0 at the sink; advertised so neighbours can route */
  uint16_t cost;         /* this node's own cost to reach the sink            */
  uint8_t  nextHop;
  uint8_t  aliveMask;    /* neighbours this node can currently hear           */
  uint16_t schedVersion;
  uint16_t retxTotal;
  uint8_t  alarm;
  uint8_t  flags;
} HelloPayload;          /* = 12 bytes                                        */

/* --- serialisation ------------------------------------------------------- */

/* Fill a header with the common fields.  `durationUs` and `seq` are set by
 * the MAC layer immediately before transmission.                            */
void   wsnInitHeader(WsnHeader *h, uint8_t type, uint8_t src,
                     uint8_t dst, uint8_t finalDst);

/* Pack header + payload + CRC into `out`.  Returns the total length, or 0 on
 * overflow.                                                                 */
size_t wsnSerialize(uint8_t *out, size_t outCap,
                    const WsnHeader *h, const void *payload, size_t payloadLen);

/* Validate magic, version, length and CRC.  On success fills `h`, points
 * `payload` at the payload bytes and sets `payloadLen`.                     */
bool   wsnParse(const uint8_t *in, size_t len,
                WsnHeader *h, const uint8_t **payload, size_t *payloadLen);

/* Expected payload size for a frame type, or 0 if variable/unknown.         */
size_t wsnPayloadLen(uint8_t type);

/* CRC-16/CCITT-FALSE                                                        */
uint16_t wsnCrc16(const uint8_t *data, size_t len);

/* Append this node to the header's path, if there is room.                  */
void   wsnPathAppend(WsnHeader *h, uint8_t nodeId);
bool   wsnPathContains(const WsnHeader *h, uint8_t nodeId);
/* Render the path as "1->3->4" into `out`.                                  */
void   wsnPathToString(const WsnHeader *h, char *out, size_t cap);

#endif /* WSN_PROTOCOL_H */
