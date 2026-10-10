#pragma once

#include <helpers/AdvertDataHelpers.h>
#include <helpers/UTF8Helpers.h>
#include <string.h>

/*
 * Beacon advert format (see docs/plan/beacon-project.md).
 *
 * A beacon is a zero-hop, signed ADV_TYPE_SENSOR advert.  The packet's timestamp field carries a monotonic
 * transmit counter (beacons have no clock).  app_data layout:
 *
 *   [flags][feat1: battery mV][feat2: BEACON_FEAT2_MARKER][name...\0][reserved bytes]
 *
 * The name is optional.  AdvertDataParser treats everything after the feature fields as the name, so when a
 * name is present it is NUL terminated and the reserved bytes follow the NUL; C-string consumers stop at it.
 * Without a name the reserved bytes follow feat2 directly and are ignored by the parser (no name flag).
 *
 * The marker is not a security control, it only lets repeaters tell beacons from real sensors.
 */

#define BEACON_FEAT2_MARKER      0xBE01   // high byte: 'beacon', low byte: format version

#ifndef BEACON_RESERVED_BYTES
  #define BEACON_RESERVED_BYTES  8        // room for future data, plan calls for 8-16
#endif

// flags(1) + feat1(2) + feat2(2) + name terminator(1) + reserved
#define BEACON_MAX_NAME_LEN   (MAX_ADVERT_DATA_SIZE - 6 - BEACON_RESERVED_BYTES)

/*
 * Default name: "beacon-" + the first 3 bytes of the beacon's public key in lowercase hex ("beacon-f5b165"), so names are
 * unique out of the box and follow the key.  Used whenever no explicit name has been set.
 */
#define BEACON_DEFAULT_NAME_LEN   13
static_assert(BEACON_DEFAULT_NAME_LEN <= BEACON_MAX_NAME_LEN, "default name must fit the advert");

static inline void beaconDefaultName(char dest[BEACON_DEFAULT_NAME_LEN + 1], const uint8_t* pub_key) {
  static const char hex[] = "0123456789abcdef";
  memcpy(dest, "beacon-", 7);
  for (int i = 0; i < 3; i++) {
    dest[7 + i * 2] = hex[pub_key[i] >> 4];
    dest[8 + i * 2] = hex[pub_key[i] & 15];
  }
  dest[BEACON_DEFAULT_NAME_LEN] = 0;
}

static inline bool beaconIsBeaconAdvert(const AdvertDataParser& parser) {
  return parser.isValid() && parser.getType() == ADV_TYPE_SENSOR && parser.getFeat2() == BEACON_FEAT2_MARKER;
}

/**
 * \brief  encode beacon app_data.
 * \param app_data  dest array, must be MAX_ADVERT_DATA_SIZE
 * \param batt_mv   battery voltage in mV, clamped to a minimum of 1 as zero would omit the field
 * \param name      optional short name (may be NULL or empty), truncated to BEACON_MAX_NAME_LEN
 * \returns  the encoded length in bytes
 */
static inline uint8_t beaconBuildAppData(uint8_t app_data[], uint16_t batt_mv, const char* name) {
  char short_name[BEACON_MAX_NAME_LEN + 1];
  size_t n = mesh::validUtf8PrefixLength(name, BEACON_MAX_NAME_LEN);
  if (n > 0) memcpy(short_name, name, n);
  short_name[n] = 0;

  AdvertDataBuilder builder(ADV_TYPE_SENSOR, short_name);
  builder.setFeat1(batt_mv ? batt_mv : 1);
  builder.setFeat2(BEACON_FEAT2_MARKER);
  uint8_t len = builder.encodeTo(app_data);

  if (app_data[0] & ADV_NAME_MASK) {
    app_data[len++] = 0;   // end of name
  }
  memset(&app_data[len], 0, BEACON_RESERVED_BYTES);
  len += BEACON_RESERVED_BYTES;
  return len;
}
