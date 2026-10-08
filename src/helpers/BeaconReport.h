#pragma once

#include <stdint.h>
#include <string.h>
#include <MeshCore.h>

/*
 * Beacon report format (see docs/plan/beacon-project.md).
 *
 * A beacon repeater batches the beacon adverts it hears and publishes them as one PAYLOAD_TYPE_GRP_DATA packet on
 * the private report channel.  The GRP_DATA data_type is BEACON_REPORT_DATA_TYPE and the data is:
 *
 *   header (10 bytes):  [version:1][repeater pub key prefix:8][entry count:1]
 *   entry  (16 bytes):  [beacon pub key prefix:8][beacon counter:4 LE][rssi:1 int8 dBm][snr:1 int8, x4][batt mV:2 LE]
 *
 * Prefixes are 8 bytes so that a matching key cannot be brute-forced; the base maps them to full keys through its
 * allowlist and to repeater locations through its repeater table.
 */

#define BEACON_REPORT_DATA_TYPE   0xFFBE   // dev range 0xFF00-0xFFFF, request an allocation once the project works
#define BEACON_REPORT_VERSION     1

#define BEACON_REPORT_ID_LEN      8
#define BEACON_REPORT_HEADER_LEN  (1 + BEACON_REPORT_ID_LEN + 1)
#define BEACON_REPORT_ENTRY_LEN   (BEACON_REPORT_ID_LEN + 4 + 1 + 1 + 2)
#define BEACON_REPORT_MAX_ENTRIES ((MAX_GROUP_DATA_LENGTH - BEACON_REPORT_HEADER_LEN) / BEACON_REPORT_ENTRY_LEN)

struct BeaconObservation {
  uint8_t  beacon_id[BEACON_REPORT_ID_LEN];   // beacon public key prefix
  uint32_t counter;
  int8_t   rssi;                              // dBm
  int8_t   snr;                               // dB x 4
  uint16_t batt_mv;
};

static inline int8_t beaconClampInt8(float v) {
  if (v > 127.0f) return 127;
  if (v < -128.0f) return -128;
  return (int8_t)(v < 0 ? v - 0.5f : v + 0.5f);   // round to nearest
}

/** \brief  encode one observation into BEACON_REPORT_ENTRY_LEN bytes */
static inline void beaconEncodeEntry(uint8_t* dest, const BeaconObservation& o) {
  memcpy(dest, o.beacon_id, BEACON_REPORT_ID_LEN);
  dest[8]  = (uint8_t)(o.counter);
  dest[9]  = (uint8_t)(o.counter >> 8);
  dest[10] = (uint8_t)(o.counter >> 16);
  dest[11] = (uint8_t)(o.counter >> 24);
  dest[12] = (uint8_t)o.rssi;
  dest[13] = (uint8_t)o.snr;
  dest[14] = (uint8_t)(o.batt_mv);
  dest[15] = (uint8_t)(o.batt_mv >> 8);
}

static inline void beaconDecodeEntry(BeaconObservation& o, const uint8_t* src) {
  memcpy(o.beacon_id, src, BEACON_REPORT_ID_LEN);
  o.counter = (uint32_t)src[8] | ((uint32_t)src[9] << 8) | ((uint32_t)src[10] << 16) | ((uint32_t)src[11] << 24);
  o.rssi = (int8_t)src[12];
  o.snr = (int8_t)src[13];
  o.batt_mv = (uint16_t)src[14] | ((uint16_t)src[15] << 8);
}

/**
 * \brief  encode a report batch.
 * \param dest  at least BEACON_REPORT_HEADER_LEN + count * BEACON_REPORT_ENTRY_LEN bytes
 * \param repeater_key  the repeater's public key (only the first BEACON_REPORT_ID_LEN bytes are sent)
 * \returns  the encoded length, or 0 if count is out of range
 */
static inline int beaconEncodeReport(uint8_t* dest, const uint8_t* repeater_key, const BeaconObservation* obs, int count) {
  if (count < 1 || count > BEACON_REPORT_MAX_ENTRIES) return 0;
  int n = 0;
  dest[n++] = BEACON_REPORT_VERSION;
  memcpy(&dest[n], repeater_key, BEACON_REPORT_ID_LEN); n += BEACON_REPORT_ID_LEN;
  dest[n++] = (uint8_t)count;
  for (int i = 0; i < count; i++) {
    beaconEncodeEntry(&dest[n], obs[i]);
    n += BEACON_REPORT_ENTRY_LEN;
  }
  return n;
}

/**
 * \brief  decode a report batch (for the base).
 * \param repeater_id  OUT - BEACON_REPORT_ID_LEN bytes
 * \param obs  OUT - up to max_obs entries
 * \returns  the number of entries decoded, or -1 if the data is malformed or an unknown version
 */
static inline int beaconDecodeReport(const uint8_t* data, int len, uint8_t* repeater_id, BeaconObservation* obs, int max_obs) {
  if (len < BEACON_REPORT_HEADER_LEN || data[0] != BEACON_REPORT_VERSION) return -1;
  int count = data[1 + BEACON_REPORT_ID_LEN];
  if (count > max_obs || len < BEACON_REPORT_HEADER_LEN + count * BEACON_REPORT_ENTRY_LEN) return -1;
  memcpy(repeater_id, &data[1], BEACON_REPORT_ID_LEN);
  for (int i = 0; i < count; i++) {
    beaconDecodeEntry(obs[i], &data[BEACON_REPORT_HEADER_LEN + i * BEACON_REPORT_ENTRY_LEN]);
  }
  return count;
}

/**
 * Holds observations until they are flushed.  One batch is one packet; add() returns false when it is full, and the
 * caller should flush before adding more.
 */
class BeaconReportBatch {
  BeaconObservation _obs[BEACON_REPORT_MAX_ENTRIES];
  int _count = 0;
public:
  bool add(const BeaconObservation& o) {
    if (_count >= BEACON_REPORT_MAX_ENTRIES) return false;
    _obs[_count++] = o;
    return true;
  }
  int count() const { return _count; }
  bool isFull() const { return _count >= BEACON_REPORT_MAX_ENTRIES; }
  void clear() { _count = 0; }
  int encode(uint8_t* dest, const uint8_t* repeater_key) const {
    return beaconEncodeReport(dest, repeater_key, _obs, _count);
  }
};
