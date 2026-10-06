#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>
#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#endif
#include <helpers/IdentityStore.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/BeaconAdvert.h>
#include <target.h>

#ifndef BEACON_NAME
  #define BEACON_NAME  "beacon"
#endif
#ifndef BEACON_INTERVAL_SECS
  #define BEACON_INTERVAL_SECS  300       // 5 minutes
#endif
#ifndef BEACON_JITTER_PCT
  #define BEACON_JITTER_PCT  10           // +/- this percent of the interval, so drifting beacons don't stay collided
#endif
// Radio defaults come from the LORA_* build flags and can be changed at runtime with the CLI.
#ifndef LORA_CR
  #define LORA_CR  5
#endif
#define BEACON_MIN_TX_POWER_DBM  -9
#define BEACON_MAX_TX_POWER_DBM  22

#define BEACON_MIN_INTERVAL_SECS  10
#define BEACON_MAX_INTERVAL_SECS  86400

// Counter is persisted with reserve-ahead: only every COUNTER_RESERVE sends (~once a day at 5 minutes) to spare flash.
#define BEACON_COUNTER_RESERVE  256

#define BEACON_PREFS_FILE   "/beacon_prefs"
#define BEACON_PREFS_MAGIC  0xBEAC0002

/**
 * The advert timestamp field has no wall clock to carry, so it carries a monotonic transmit counter instead.
 * Mesh::createAdvert() stamps whatever getCurrentTime() returns, and signs it.
 */
class BeaconCounterClock : public mesh::RTCClock {
  uint32_t _counter = 0;
public:
  uint32_t getCurrentTime() override { return _counter; }
  void setCurrentTime(uint32_t time) override { }   // no clock to set
  uint32_t next() { return ++_counter; }
  void resume(uint32_t counter) { _counter = counter; }
};

// Beacons only transmit, so there is nothing to de-duplicate.
class NullMeshTables : public mesh::MeshTables {
public:
  bool wasSeen(const mesh::Packet* packet) override { return false; }
  void markSeen(const mesh::Packet* packet) override { }
  void clear(const mesh::Packet* packet) override { }
};

struct BeaconPrefs {
  uint32_t magic;
  uint32_t interval_secs;
  uint32_t counter_ceiling;   // highest counter value that may have been transmitted before the next save
  float freq;                 // MHz
  float bw;                   // kHz
  uint8_t sf;
  uint8_t cr;
  int8_t tx_power_dbm;
  char name[BEACON_MAX_NAME_LEN + 1];
};

class Beacon : public mesh::Mesh {
  FILESYSTEM* _fs;
  BeaconCounterClock& _clock;
  BeaconPrefs _prefs;
  bool _prefs_loaded;
  const char* _last_error = "";

  bool savePrefs();

public:
  Beacon(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, BeaconCounterClock& clock,
         mesh::PacketManager& mgr, mesh::MeshTables& tables)
    : mesh::Mesh(radio, ms, rng, clock, mgr, tables), _fs(NULL), _clock(clock), _prefs_loaded(false) { }

  // Beacons never receive, so the Dispatcher loop is not used and this is never called.
  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override { return ACTION_RELEASE; }

  /** \brief load prefs and resume the counter. Returns false if prefs were missing (counter restarts from zero). */
  bool begin(FILESYSTEM* fs);

  /** \brief build, sign and transmit one zero-hop advert, then put the radio back to sleep. */
  bool sendAdvert();

  /** \returns  short reason for the most recent sendAdvert() failure */
  const char* getLastError() const { return _last_error; }

  /** \returns  ms until the next transmit, including random jitter */
  uint32_t nextIntervalMillis();

  uint32_t getCounter() const { return _clock.getCurrentTime(); }
  uint32_t getIntervalSecs() const { return _prefs.interval_secs; }
  const char* getName() const { return _prefs.name; }

  float getFreq() const { return _prefs.freq; }
  float getBandwidth() const { return _prefs.bw; }
  uint8_t getSpreadFactor() const { return _prefs.sf; }
  uint8_t getCodingRate() const { return _prefs.cr; }
  int8_t getTxPower() const { return _prefs.tx_power_dbm; }

  /** \brief push the current radio prefs to the radio, then back to sleep */
  void applyRadio();

  /** \returns  false (nothing changed) if any parameter is out of range */
  bool setRadio(float freq, float bw, uint8_t sf, uint8_t cr);
  bool setTxPower(int8_t dbm);

  bool setIntervalSecs(uint32_t secs);
  bool setName(const char* name);
};
