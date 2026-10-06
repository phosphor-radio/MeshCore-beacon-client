#include "Beacon.h"

static uint16_t readBattMilliVolts() {
#ifdef VBAT_ENABLE
  digitalWrite(VBAT_ENABLE, LOW);   // enable the divider only while sampling, it leaks current otherwise
  delay(5);
#endif
  uint16_t mv = board.getBattMilliVolts();
#ifdef VBAT_ENABLE
  digitalWrite(VBAT_ENABLE, HIGH);
#endif
  return mv;
}

static bool radioParamsValid(float freq, float bw, uint8_t sf, uint8_t cr, int8_t tx_dbm) {
  return freq >= 150.0f && freq <= 2500.0f && bw >= 7.0f && bw <= 500.0f && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8
      && tx_dbm >= BEACON_MIN_TX_POWER_DBM && tx_dbm <= BEACON_MAX_TX_POWER_DBM;
}

bool Beacon::savePrefs() {
  _fs->remove(BEACON_PREFS_FILE);
  File file = _fs->open(BEACON_PREFS_FILE, FILE_O_WRITE);
  if (!file) return false;
  bool ok = file.write((const uint8_t*)&_prefs, sizeof(_prefs)) == sizeof(_prefs);
  file.close();
  return ok;
}

bool Beacon::begin(FILESYSTEM* fs) {
  _fs = fs;

  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.magic = BEACON_PREFS_MAGIC;
  _prefs.interval_secs = BEACON_INTERVAL_SECS;
  strncpy(_prefs.name, BEACON_NAME, sizeof(_prefs.name) - 1);
  _prefs.freq = LORA_FREQ;
  _prefs.bw = LORA_BW;
  _prefs.sf = LORA_SF;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;

  _prefs_loaded = false;
  if (_fs->exists(BEACON_PREFS_FILE)) {
    File file = _fs->open(BEACON_PREFS_FILE);
    if (file) {
      BeaconPrefs stored;
      if (file.read((uint8_t*)&stored, sizeof(stored)) == sizeof(stored) && stored.magic == BEACON_PREFS_MAGIC
          && stored.interval_secs >= BEACON_MIN_INTERVAL_SECS && stored.interval_secs <= BEACON_MAX_INTERVAL_SECS) {
        stored.name[sizeof(stored.name) - 1] = 0;
        if (!radioParamsValid(stored.freq, stored.bw, stored.sf, stored.cr, stored.tx_power_dbm)) {
          stored.freq = _prefs.freq; stored.bw = _prefs.bw; stored.sf = _prefs.sf;   // keep counter, fall back to build defaults
          stored.cr = _prefs.cr; stored.tx_power_dbm = _prefs.tx_power_dbm;
        }
        _prefs = stored;
        _prefs_loaded = true;
      }
      file.close();
    }
  }

  // Resume from the persisted ceiling, which is >= any counter already transmitted, then reserve the next block.
  _clock.resume(_prefs.counter_ceiling);
  _prefs.counter_ceiling += BEACON_COUNTER_RESERVE;
  savePrefs();

  mesh::Mesh::begin();
  applyRadio();
  return _prefs_loaded;
}

uint32_t Beacon::nextIntervalMillis() {
  uint32_t base = _prefs.interval_secs * 1000UL;
  uint32_t spread = base / 100 * BEACON_JITTER_PCT;
  return base - spread + (uint32_t)random(0, 2 * spread + 1);
}

bool Beacon::sendAdvert() {
  uint32_t counter = _clock.next();
  if (counter >= _prefs.counter_ceiling) {   // persist the new ceiling *before* using values beyond the old one
    _prefs.counter_ceiling = counter + BEACON_COUNTER_RESERVE;
    savePrefs();
  }

  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = beaconBuildAppData(app_data, readBattMilliVolts(), _prefs.name);

  mesh::Packet* pkt = createAdvert(self_id, app_data, app_data_len);   // stamps + signs the counter
  if (pkt == NULL) { _last_error = "no packet"; return false; }

  // same wire format as Mesh::sendZeroHop(), but sent straight to the radio so it never enters RX
  pkt->header |= ROUTE_TYPE_DIRECT;
  pkt->path_len = 0;

  uint8_t raw[MAX_TRANS_UNIT];
  int len = pkt->writeTo(raw);
  releasePacket(pkt);

  // no listen-before-talk, the beacon has no receiver to listen with
  uint32_t started = millis();
  uint32_t timeout = radio_driver.getEstAirtimeFor(len) * 3 / 2 + 100;
  radio_driver.wakeUp();   // startTransmit() does not leave sleep itself, unlike startReceive()
  bool ok = radio_driver.startSendRaw(raw, len);
  if (!ok) {
    _last_error = "startTransmit failed";
  } else {
    while (!radio_driver.isSendComplete()) {
      if (millis() - started > timeout) { ok = false; _last_error = "tx done timeout"; break; }
      delay(2);   // MCU idles between polls
    }
    radio_driver.onSendFinished();
  }
  radio_driver.sleepKeepConfig();
  return ok;
}

bool Beacon::setIntervalSecs(uint32_t secs) {
  if (secs < BEACON_MIN_INTERVAL_SECS || secs > BEACON_MAX_INTERVAL_SECS) return false;
  _prefs.interval_secs = secs;
  return savePrefs();
}

bool Beacon::setName(const char* name) {
  if (!AdvertDataParser::isValidName(name)) return false;
  size_t n = mesh::validUtf8PrefixLength(name, BEACON_MAX_NAME_LEN);
  if (name[n] != 0) return false;   // too long or not valid UTF-8
  memset(_prefs.name, 0, sizeof(_prefs.name));
  memcpy(_prefs.name, name, n);
  return savePrefs();
}

void Beacon::applyRadio() {
  radio_driver.wakeUp();
  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);
  radio_driver.sleepKeepConfig();
}

bool Beacon::setRadio(float freq, float bw, uint8_t sf, uint8_t cr) {
  if (!radioParamsValid(freq, bw, sf, cr, _prefs.tx_power_dbm)) return false;
  _prefs.freq = freq; _prefs.bw = bw; _prefs.sf = sf; _prefs.cr = cr;
  applyRadio();
  return savePrefs();
}

bool Beacon::setTxPower(int8_t dbm) {
  if (!radioParamsValid(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr, dbm)) return false;
  _prefs.tx_power_dbm = dbm;
  applyRadio();
  return savePrefs();
}
