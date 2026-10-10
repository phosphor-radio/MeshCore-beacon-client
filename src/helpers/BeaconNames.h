#pragma once

#include <stdint.h>
#include <string.h>
#include <MeshCore.h>

/*
 * Beacon name announcement format.
 *
 * A beacon advertises a name in its signed advert.  A beacon repeater that hears it tells the base station
 * "this key prefix is called this" with a second GRP_DATA message type on the private report channel, separate from
 * the observation reports in BeaconReport.h (which stay unchanged).  data_type is BEACON_NAMES_DATA_TYPE and the data is:
 *
 *   header (10 bytes):  [version:1][repeater pub key prefix:8][entry count:1]
 *   entry (variable):   [beacon pub key prefix:8][name length:1][name: UTF-8, no NUL]
 *
 * Decoder rules: an unknown version, a short header or an entry that runs past the end of the data drops the whole
 * message; trailing bytes are ignored; an entry with a zero-length name is skipped.  The encoder never emits a
 * zero-length name.  The wire format does not limit the name length (the base applies its own display cap), but a
 * beacon's name is at most BEACON_MAX_NAME_LEN bytes and the advert parser at most 31.
 */

#define BEACON_NAMES_DATA_TYPE      0xFFBF   // dev range 0xFF00-0xFFFF, like BEACON_REPORT_DATA_TYPE
#define BEACON_NAMES_VERSION        1

#define BEACON_NAMES_ID_LEN         8
#define BEACON_NAMES_HEADER_LEN     (1 + BEACON_NAMES_ID_LEN + 1)
#define BEACON_NAMES_ENTRY_OVERHEAD (BEACON_NAMES_ID_LEN + 1)   // plus the name bytes
// the most entries a datagram can hold, which only zero-length names (skipped on decode) can actually reach
#define BEACON_NAMES_MAX_ENTRIES    ((MAX_GROUP_DATA_LENGTH - BEACON_NAMES_HEADER_LEN) / BEACON_NAMES_ENTRY_OVERHEAD)

struct BeaconNameEntry {
  uint8_t id[BEACON_NAMES_ID_LEN];   // beacon public key prefix
  const uint8_t* name;               // points into the decoded buffer, not NUL terminated
  uint8_t name_len;
};

/**
 * \brief  decode a name message.
 * \param repeater_id  OUT - BEACON_NAMES_ID_LEN bytes
 * \param out  OUT - the non-empty entries, valid only while 'data' is
 * \param max_out  capacity of 'out'; a message declaring more entries than this is rejected
 * \returns  the number of entries returned in 'out', or -1 if the message is malformed or an unknown version
 */
static inline int beaconNamesDecode(const uint8_t* data, int len, uint8_t* repeater_id, BeaconNameEntry* out, int max_out) {
  if (len < BEACON_NAMES_HEADER_LEN || data[0] != BEACON_NAMES_VERSION) return -1;
  int count = data[1 + BEACON_NAMES_ID_LEN];
  if (count > max_out) return -1;

  int pos = BEACON_NAMES_HEADER_LEN;
  int n = 0;
  for (int i = 0; i < count; i++) {
    if (pos + BEACON_NAMES_ENTRY_OVERHEAD > len) return -1;
    const uint8_t* id = &data[pos];
    int name_len = data[pos + BEACON_NAMES_ID_LEN];
    pos += BEACON_NAMES_ENTRY_OVERHEAD;
    if (pos + name_len > len) return -1;
    if (name_len > 0) {
      memcpy(out[n].id, id, BEACON_NAMES_ID_LEN);
      out[n].name = &data[pos];
      out[n].name_len = (uint8_t)name_len;
      n++;
    }
    pos += name_len;
  }
  memcpy(repeater_id, &data[1], BEACON_NAMES_ID_LEN);
  return n;
}

/**
 * Holds name announcements until they are flushed.  One batch is one packet.  add() returns false when the entry does
 * not fit; the caller should flush and add again.
 */
class BeaconNameBatch {
  uint8_t _buf[MAX_GROUP_DATA_LENGTH - BEACON_NAMES_HEADER_LEN];
  int _len = 0;
  int _count = 0;
public:
  bool add(const uint8_t* id, const char* name, int name_len) {
    if (name_len < 1 || name_len > 255 || _count >= 255) return false;
    if (_len + BEACON_NAMES_ENTRY_OVERHEAD + name_len > (int)sizeof(_buf)) return false;
    memcpy(&_buf[_len], id, BEACON_NAMES_ID_LEN); _len += BEACON_NAMES_ID_LEN;
    _buf[_len++] = (uint8_t)name_len;
    memcpy(&_buf[_len], name, name_len); _len += name_len;
    _count++;
    return true;
  }
  int count() const { return _count; }
  void clear() { _len = 0; _count = 0; }

  /**
   * \param dest  at least MAX_GROUP_DATA_LENGTH bytes
   * \param repeater_key  the repeater's public key (only the first BEACON_NAMES_ID_LEN bytes are sent)
   * \returns  the encoded length, or 0 if the batch is empty
   */
  int encode(uint8_t* dest, const uint8_t* repeater_key) const {
    if (_count == 0) return 0;
    int n = 0;
    dest[n++] = BEACON_NAMES_VERSION;
    memcpy(&dest[n], repeater_key, BEACON_NAMES_ID_LEN); n += BEACON_NAMES_ID_LEN;
    dest[n++] = (uint8_t)_count;
    memcpy(&dest[n], _buf, _len);
    return n + _len;
  }
};

/** \brief  FNV-1a, used to notice a changed name without keeping the name itself */
static inline uint32_t beaconNameHash(const char* name, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= (uint8_t)name[i];
    h *= 16777619u;
  }
  return h;
}

/**
 * Decides when a repeater should announce a beacon's name: the first time it hears the beacon since boot, when the
 * name changed, and when the last announcement is older than the refresh interval.  Holds a hash of each name, not
 * the name, with least-recently-used eviction.
 *
 * Two steps so a name that could not be queued is not marked as announced: check(), then markAnnounced() once the
 * entry is in a batch.
 */
template <int N>
class BeaconNameCacheT {
public:
  enum Reason { NONE = 0, FIRST_SEEN = 1, CHANGED = 2, REFRESH = 3 };

  /**
   * \param refresh_secs  announce again when the last announcement is at least this old; 0 turns the refresh off
   * \returns  why the name should be announced, or NONE
   */
  Reason check(const uint8_t* id, uint32_t name_hash, uint32_t now_secs, uint32_t refresh_secs) {
    Entry* e = find(id);
    if (e == NULL) return FIRST_SEEN;
    e->last_used = ++_tick;
    if (e->name_hash != name_hash) return CHANGED;
    if (refresh_secs != 0 && (uint32_t)(now_secs - e->last_announced) >= refresh_secs) return REFRESH;
    return NONE;
  }

  void markAnnounced(const uint8_t* id, uint32_t name_hash, uint32_t now_secs) {
    Entry* e = find(id);
    if (e == NULL) {
      e = victim();
      memcpy(e->id, id, BEACON_NAMES_ID_LEN);
    }
    e->name_hash = name_hash;
    e->last_announced = now_secs;
    e->last_used = ++_tick;
  }

  int size() const {
    int n = 0;
    for (int i = 0; i < N; i++) if (_entries[i].last_used != 0) n++;
    return n;
  }
  void clear() { memset(_entries, 0, sizeof(_entries)); _tick = 0; }

  BeaconNameCacheT() { clear(); }

private:
  struct Entry {
    uint8_t id[BEACON_NAMES_ID_LEN];
    uint32_t name_hash;
    uint32_t last_announced;
    uint32_t last_used;   // 0 means the slot is free
  };
  Entry _entries[N];
  uint32_t _tick;

  Entry* find(const uint8_t* id) {
    for (int i = 0; i < N; i++) {
      if (_entries[i].last_used != 0 && memcmp(_entries[i].id, id, BEACON_NAMES_ID_LEN) == 0) return &_entries[i];
    }
    return NULL;
  }
  Entry* victim() {
    Entry* v = &_entries[0];
    for (int i = 0; i < N; i++) {
      if (_entries[i].last_used == 0) return &_entries[i];
      if (_entries[i].last_used < v->last_used) v = &_entries[i];
    }
    return v;
  }
};

#ifndef BEACON_NAME_CACHE_SIZE
  #define BEACON_NAME_CACHE_SIZE  64
#endif
typedef BeaconNameCacheT<BEACON_NAME_CACHE_SIZE> BeaconNameCache;
