/* =============================================================================
 *  wsn_protocol.cpp  --  Frame serialisation, parsing and integrity checking.
 * ========================================================================== */

#include "wsn_protocol.h"

/* -------------------------------------------------------------------------- */

const char *wsnFrameTypeName(uint8_t t) {
  switch (t) {
    case FT_HELLO:        return "HELLO";
    case FT_BEACON:       return "BEACON";
    case FT_DATA:         return "DATA";
    case FT_AGG:          return "AGG";
    case FT_ACK:          return "ACK";
    case FT_ROUTE_UPDATE: return "ROUTE";
    case FT_NET_STATUS:   return "STATUS";
    case FT_CMD:          return "CMD";
    case FT_SLOT_REQ:     return "SLOTREQ";
    case FT_LOC:          return "LOC";
    default:              return "???";
  }
}

const char *wsnSchedReasonName(uint8_t r) {
  switch (r) {
    case SCHED_INIT:           return "initial schedule";
    case SCHED_ALARM_GRANT:    return "alarm raised - dynamic slot granted";
    case SCHED_ALARM_RELEASE:  return "alarm cleared - dynamic slot released";
    case SCHED_NODE_SILENT:    return "node silent - slot revoked";
    case SCHED_NODE_REJOIN:    return "node rejoined - slot restored";
    case SCHED_BACKLOG_GRANT:  return "backlog reported - extra slot granted";
    case SCHED_COLLISION_TEST: return "collision test - two owners in one slot";
    default:                   return "unknown";
  }
}

const char *wsnRouteEventName(uint8_t e) {
  switch (e) {
    case RE_LINK_DOWN:     return "LINK_DOWN";
    case RE_LINK_UP:       return "LINK_UP";
    case RE_ROUTE_CHANGED: return "ROUTE_CHANGED";
    default:               return "???";
  }
}

size_t wsnPayloadLen(uint8_t type) {
  switch (type) {
    case FT_HELLO:        return sizeof(HelloPayload);
    case FT_BEACON:       return sizeof(BeaconPayload);
    case FT_DATA:         return sizeof(DataPayload);
    case FT_AGG:          return sizeof(AggPayload);
    case FT_ACK:          return sizeof(AckPayload);
    case FT_ROUTE_UPDATE: return sizeof(RoutePayload);
    case FT_NET_STATUS:   return sizeof(NetStatusPayload);
    case FT_CMD:          return sizeof(CmdPayload);
    case FT_SLOT_REQ:     return sizeof(SlotReqPayload);
    case FT_LOC:          return sizeof(LocPayload);
    default:              return 0;
  }
}

/* -----------------------------------------------------------------------------
 *  CRC-16/CCITT-FALSE  (poly 0x1021, init 0xFFFF, no reflection, no xorout).
 *
 *  ESP-NOW already carries an 802.11 FCS, so a corrupted frame rarely reaches
 *  us.  We still check our own CRC because the forced-collision demonstration
 *  deliberately produces overlapping transmissions, and because a truncated or
 *  mis-typed frame must never be interpreted as sensor data.
 * -------------------------------------------------------------------------- */
uint16_t wsnCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* -------------------------------------------------------------------------- */

void wsnInitHeader(WsnHeader *h, uint8_t type, uint8_t src,
                   uint8_t dst, uint8_t finalDst) {
  memset(h, 0, sizeof(WsnHeader));
  h->magic      = WSN_MAGIC;
  h->ver        = WSN_PROTO_VER;
  h->type       = type;
  h->src        = src;
  h->dst        = dst;
  h->finalDst   = finalDst;
  h->hop        = 0;
  h->ttl        = WSN_DEFAULT_TTL;
  h->seq        = 0;
  h->txTimeMs   = 0;
  h->durationUs = 0;
  h->pathLen    = 0;
  wsnPathAppend(h, src);
}

size_t wsnSerialize(uint8_t *out, size_t outCap,
                    const WsnHeader *h, const void *payload, size_t payloadLen) {
  const size_t total = WSN_HDR_LEN + payloadLen + 2;
  if (total > outCap || total > WSN_MAX_FRAME) return 0;

  memcpy(out, h, WSN_HDR_LEN);
  if (payloadLen && payload) memcpy(out + WSN_HDR_LEN, payload, payloadLen);

  const uint16_t crc = wsnCrc16(out, WSN_HDR_LEN + payloadLen);
  out[WSN_HDR_LEN + payloadLen]     = (uint8_t)(crc & 0xFF);
  out[WSN_HDR_LEN + payloadLen + 1] = (uint8_t)(crc >> 8);
  return total;
}

bool wsnParse(const uint8_t *in, size_t len,
              WsnHeader *h, const uint8_t **payload, size_t *payloadLen) {
  if (len < (size_t)(WSN_HDR_LEN + 2)) return false;
  if (in[0] != WSN_MAGIC || in[1] != WSN_PROTO_VER) return false;

  const size_t plen = len - WSN_HDR_LEN - 2;
  const uint16_t want = (uint16_t)in[WSN_HDR_LEN + plen] |
                        ((uint16_t)in[WSN_HDR_LEN + plen + 1] << 8);
  if (wsnCrc16(in, WSN_HDR_LEN + plen) != want) return false;

  memcpy(h, in, WSN_HDR_LEN);

  /* Reject frames whose payload is not the size their type demands.  A frame
   * type with a known fixed size that arrives short would otherwise be read
   * past its end.                                                            */
  const size_t expect = wsnPayloadLen(h->type);
  if (expect && plen != expect) return false;

  if (payload)    *payload    = in + WSN_HDR_LEN;
  if (payloadLen) *payloadLen = plen;
  return true;
}

/* -------------------------------------------------------------------------- */

void wsnPathAppend(WsnHeader *h, uint8_t nodeId) {
  if (h->pathLen < sizeof(h->path)) h->path[h->pathLen++] = nodeId;
}

bool wsnPathContains(const WsnHeader *h, uint8_t nodeId) {
  for (uint8_t i = 0; i < h->pathLen && i < sizeof(h->path); i++) {
    if (h->path[i] == nodeId) return true;
  }
  return false;
}

void wsnPathToString(const WsnHeader *h, char *out, size_t cap) {
  if (!cap) return;
  out[0] = '\0';
  size_t used = 0;
  for (uint8_t i = 0; i < h->pathLen && i < sizeof(h->path); i++) {
    int n = snprintf(out + used, cap - used, "%s%u", i ? "->" : "", h->path[i]);
    if (n < 0 || (size_t)n >= cap - used) break;
    used += (size_t)n;
  }
}
