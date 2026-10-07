#include "Beacon.h"

#define BEACON_FW_VERSION  "beacon-0.1"

static BeaconCounterClock counter_clock;
static StdRNG fast_rng;
static NullMeshTables tables;
static StaticPoolPacketManager packet_mgr(2);
static ArduinoMillis millis_clock;

static Beacon beacon(radio_driver, millis_clock, fast_rng, counter_clock, packet_mgr, tables);

static uint32_t next_tx_at;
static char command[80];

static void halt() {
  while (1) delay(1000);
}

static void handleCommand(char* cmd) {
  if (strcmp(cmd, "help") == 0) {
    Serial.println("  ver | pubkey | advert | reboot");
    Serial.println("  get interval|name|counter|batt|radio|tx");
    Serial.println("  set interval <secs> | set name <name>");
    Serial.println("  set radio <freq>,<bw>,<sf>,<cr> | set tx <dbm>");
  } else if (strcmp(cmd, "ver") == 0) {
    Serial.println("  -> " BEACON_FW_VERSION);
  } else if (strcmp(cmd, "pubkey") == 0) {
    Serial.print("  -> "); mesh::Utils::printHex(Serial, beacon.self_id.pub_key, PUB_KEY_SIZE); Serial.println();
  } else if (strcmp(cmd, "advert") == 0) {
    if (beacon.sendAdvert()) Serial.println("  -> sent");
    else Serial.printf("  -> ERROR: send failed (%s)\r\n", beacon.getLastError());
  } else if (strcmp(cmd, "reboot") == 0) {
    Serial.println("  -> rebooting");
    Serial.flush();
    board.reboot();
  } else if (strcmp(cmd, "get interval") == 0) {
    Serial.printf("  -> %lu secs\r\n", beacon.getIntervalSecs());
  } else if (strcmp(cmd, "get name") == 0) {
    Serial.printf("  -> %s\r\n", beacon.getName());
  } else if (strcmp(cmd, "get radio") == 0) {
    Serial.printf("  -> %.3f,%.1f,%d,%d\r\n", beacon.getFreq(), beacon.getBandwidth(), beacon.getSpreadFactor(), beacon.getCodingRate());
  } else if (strcmp(cmd, "get tx") == 0) {
    Serial.printf("  -> %d dBm\r\n", beacon.getTxPower());
  } else if (strcmp(cmd, "get counter") == 0) {
    Serial.printf("  -> %lu\r\n", beacon.getCounter());
  } else if (strcmp(cmd, "get batt") == 0) {
    Serial.printf("  -> %u mV\r\n", (unsigned)board.getBattMilliVolts());
  } else if (memcmp(cmd, "set interval ", 13) == 0) {
    if (beacon.setIntervalSecs(strtoul(&cmd[13], NULL, 10))) {
      next_tx_at = millis() + beacon.nextIntervalMillis();
      Serial.println("  -> OK");
    } else {
      Serial.printf("  -> ERROR: interval must be %d-%d secs\r\n", BEACON_MIN_INTERVAL_SECS, BEACON_MAX_INTERVAL_SECS);
    }
  } else if (memcmp(cmd, "set radio ", 10) == 0) {
    const char* parts[4];
    int num = mesh::Utils::parseTextParts(&cmd[10], parts, 4);
    bool ok = num == 4 && beacon.setRadio(strtof(parts[0], NULL), strtof(parts[1], NULL), atoi(parts[2]), atoi(parts[3]));
    Serial.println(ok ? "  -> OK" : "  -> ERROR: usage set radio <freq MHz>,<bw kHz>,<sf 5-12>,<cr 5-8>");
  } else if (memcmp(cmd, "set tx ", 7) == 0) {
    Serial.println(beacon.setTxPower(atoi(&cmd[7])) ? "  -> OK" : "  -> ERROR: tx power must be -9 to 22 dBm");
  } else if (memcmp(cmd, "set name ", 9) == 0) {
    Serial.println(beacon.setName(&cmd[9]) ? "  -> OK" : "  -> ERROR: invalid name");
  } else {
    Serial.println("  -> unknown command, try 'help'");
  }
}

static void pollSerial() {
  static int len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) {
        command[len] = 0;
        handleCommand(command);
        len = 0;
      }
    } else if (len < (int)sizeof(command) - 1) {
      command[len++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);

  board.begin();
  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

  InternalFS.begin();
  IdentityStore store(InternalFS, "");
  bool has_identity = store.load("_main", beacon.self_id);
  if (!has_identity) {
    beacon.self_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (beacon.self_id.pub_key[0] == 0x00 || beacon.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      beacon.self_id = radio_new_identity(); count++;
    }
    store.save("_main", beacon.self_id);
  }

  bool prefs_ok = beacon.begin(&InternalFS);
  if (has_identity && !prefs_ok) {
    // key survived but the counter did not: the base will reject this beacon until its high-water mark is reset
    Serial.println("WARNING: counter state missing, restarting from zero. Reset this beacon at the base.");
  }

  if (Serial) {
    Serial.print("Beacon ID: "); mesh::Utils::printHex(Serial, beacon.self_id.pub_key, PUB_KEY_SIZE); Serial.println();
  }

  next_tx_at = millis() + random(1000, 5000);   // first advert shortly after boot, randomised so a batch of beacons don't collide
}

void loop() {
  bool usb = Serial;   // true only while a host has the port open
  if (usb) pollSerial();

  if ((int32_t)(millis() - next_tx_at) >= 0) {
    bool ok = beacon.sendAdvert();
    if (usb) Serial.printf("TX counter=%lu %s%s%s\r\n", beacon.getCounter(), ok ? "ok" : "FAILED (", ok ? "" : beacon.getLastError(), ok ? "" : ")");
    next_tx_at = millis() + beacon.nextIntervalMillis();
  }

  int32_t wait = (int32_t)(next_tx_at - millis());
  if (wait < 1) wait = 1;
  if (usb && wait > 50) wait = 50;   // stay responsive to the CLI while a host is connected
  delay(wait);                       // FreeRTOS tickless idle: MCU sleeps until the timer
}
