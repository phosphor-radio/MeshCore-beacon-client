#include <gtest/gtest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include <helpers/BeaconReport.h>

/*
 * Golden vectors for the beacon report wire format (src/helpers/BeaconReport.h).
 *
 * The hex strings below were derived with an independent encoder (Python struct) from the layout in BeaconReport.h,
 * not by running this code, so they guard the format rather than mirror the implementation.
 *
 * The base station (separate repository) consumes the same vectors.  Set BEACON_REPORT_VECTORS_OUT to a file path to
 * have this test write them as JSON:
 *
 *   BEACON_REPORT_VECTORS_OUT=/path/beacon_report_v1.json pio test -e native -f test_beacon_report
 */

namespace {

struct Obs { uint8_t id[BEACON_REPORT_ID_LEN]; uint32_t counter; int8_t rssi; int8_t snr; uint16_t batt_mv; };

struct ValidCase {
  const char* name;
  std::vector<Obs> obs;
  const char* hex;
};

// repeater public key used by every case: bytes 0x00..0x1F, only the first 8 are transmitted
static uint8_t repeater_key[PUB_KEY_SIZE];

static const char* SINGLE_HEX =
  "01000102030405060701a0a1a2a3a4a5a6a7010000009feb930f";

static std::vector<ValidCase> validCases() {
  std::vector<ValidCase> v;
  v.push_back({"single_entry",
    {{{0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7}, 1, -97, -21, 3987}},
    SINGLE_HEX});
  v.push_back({"extremes",
    {{{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}, 0xFFFFFFFFu, -128, 127, 65535},
     {{0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00}, 0, 127, -128, 0}},
    "010001020304050607021122334455667788ffffffff807fffff99aabbccddeeff00000000007f800000"});
  v.push_back({"counter_byte_order",
    {{{1,2,3,4,5,6,7,8}, 0x01020304u, 0, 0, 0x0102}},
    "0100010203040506070101020304050607080403020100000201"});

  ValidCase full = {"full_batch", {},
    "010001020304050607090001020304050607e8030000c428e40c10111213141516170d040000bf1d480d202122232425262732040000ba12ac0d"
    "303132333435363757040000b507100e40414243444546477c040000b0fc740e5051525354555657a1040000abf1d80e6061626364656667c604"
    "0000a6e63c0f7071727374757677eb040000a1dba00f8081828384858687100500009cd00410"};
  for (int i = 0; i < 9; i++) {
    Obs o;
    for (int j = 0; j < 8; j++) o.id[j] = (uint8_t)(0x10 * i + j);
    o.counter = 1000 + 37 * i;
    o.rssi = (int8_t)(-60 - 5 * i);
    o.snr = (int8_t)(40 - 11 * i);
    o.batt_mv = (uint16_t)(3300 + 100 * i);
    full.obs.push_back(o);
  }
  v.push_back(full);
  return v;
}

struct DecodeCase {
  const char* name;
  const char* hex;
  int expect_count;   // -1 means the decoder must reject it
};

// decode-only cases: malformed input that must be rejected, and odd-but-accepted input
static std::vector<DecodeCase> decodeCases() {
  return {
    {"reject_empty", "", -1},
    {"reject_short_header", "010001020304050607", -1},
    {"reject_version_0", "00000102030405060701a0a1a2a3a4a5a6a7010000009feb930f", -1},
    {"reject_version_2", "02000102030405060701a0a1a2a3a4a5a6a7010000009feb930f", -1},
    {"reject_version_255", "ff000102030405060701a0a1a2a3a4a5a6a7010000009feb930f", -1},
    {"reject_count_exceeds_data", "01000102030405060702a0a1a2a3a4a5a6a7010000009feb930f", -1},
    {"reject_truncated_entry", "01000102030405060701a0a1a2a3a4a5a6a7010000009feb93", -1},
    {"accept_empty_batch", "01000102030405060700", 0},
    {"accept_trailing_bytes", "01000102030405060701a0a1a2a3a4a5a6a7010000009feb930f0000", 1},
  };
}

static std::vector<uint8_t> fromHex(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    char b[3] = {hex[i], hex[i + 1], 0};
    out.push_back((uint8_t)strtoul(b, NULL, 16));
  }
  return out;
}

static std::string toHex(const uint8_t* d, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
  return s;
}

static BeaconObservation toObservation(const Obs& o) {
  BeaconObservation b;
  memcpy(b.beacon_id, o.id, BEACON_REPORT_ID_LEN);
  b.counter = o.counter; b.rssi = o.rssi; b.snr = o.snr; b.batt_mv = o.batt_mv;
  return b;
}

class BeaconReportTest : public ::testing::Test {
protected:
  void SetUp() override { for (int i = 0; i < PUB_KEY_SIZE; i++) repeater_key[i] = (uint8_t)i; }
};

}  // namespace

TEST_F(BeaconReportTest, ConstantsMatchTheDocumentedLayout) {
  EXPECT_EQ(8, BEACON_REPORT_ID_LEN);
  EXPECT_EQ(10, BEACON_REPORT_HEADER_LEN);
  EXPECT_EQ(16, BEACON_REPORT_ENTRY_LEN);
  EXPECT_EQ(9, BEACON_REPORT_MAX_ENTRIES);
  EXPECT_EQ(1, BEACON_REPORT_VERSION);
  EXPECT_EQ(0xFFBE, BEACON_REPORT_DATA_TYPE);
  EXPECT_LE(BEACON_REPORT_HEADER_LEN + BEACON_REPORT_MAX_ENTRIES * BEACON_REPORT_ENTRY_LEN, MAX_GROUP_DATA_LENGTH);
}

TEST_F(BeaconReportTest, EncodesGoldenVectors) {
  for (const auto& c : validCases()) {
    std::vector<BeaconObservation> obs;
    for (const auto& o : c.obs) obs.push_back(toObservation(o));
    uint8_t buf[MAX_GROUP_DATA_LENGTH];
    int n = beaconEncodeReport(buf, repeater_key, obs.data(), (int)obs.size());
    EXPECT_EQ(c.hex, toHex(buf, n)) << c.name;
  }
}

TEST_F(BeaconReportTest, DecodesGoldenVectors) {
  for (const auto& c : validCases()) {
    std::vector<uint8_t> data = fromHex(c.hex);
    uint8_t rid[BEACON_REPORT_ID_LEN];
    BeaconObservation out[BEACON_REPORT_MAX_ENTRIES];
    int n = beaconDecodeReport(data.data(), (int)data.size(), rid, out, BEACON_REPORT_MAX_ENTRIES);
    ASSERT_EQ((int)c.obs.size(), n) << c.name;
    EXPECT_EQ(0, memcmp(rid, repeater_key, BEACON_REPORT_ID_LEN)) << c.name;
    for (int i = 0; i < n; i++) {
      EXPECT_EQ(0, memcmp(out[i].beacon_id, c.obs[i].id, BEACON_REPORT_ID_LEN)) << c.name << " #" << i;
      EXPECT_EQ(c.obs[i].counter, out[i].counter) << c.name << " #" << i;
      EXPECT_EQ(c.obs[i].rssi, out[i].rssi) << c.name << " #" << i;
      EXPECT_EQ(c.obs[i].snr, out[i].snr) << c.name << " #" << i;
      EXPECT_EQ(c.obs[i].batt_mv, out[i].batt_mv) << c.name << " #" << i;
    }
  }
}

TEST_F(BeaconReportTest, RejectsAndAcceptsDecodeOnlyCases) {
  for (const auto& c : decodeCases()) {
    std::vector<uint8_t> data = fromHex(c.hex);
    uint8_t rid[BEACON_REPORT_ID_LEN];
    BeaconObservation out[BEACON_REPORT_MAX_ENTRIES];
    int n = beaconDecodeReport(data.data(), (int)data.size(), rid, out, BEACON_REPORT_MAX_ENTRIES);
    EXPECT_EQ(c.expect_count, n) << c.name;
  }
}

TEST_F(BeaconReportTest, DecoderHonoursCallerCapacity) {
  std::vector<uint8_t> data = fromHex(validCases()[3].hex);   // full_batch, 9 entries
  uint8_t rid[BEACON_REPORT_ID_LEN];
  BeaconObservation out[8];
  EXPECT_EQ(-1, beaconDecodeReport(data.data(), (int)data.size(), rid, out, 8));
}

TEST_F(BeaconReportTest, EncoderRefusesEmptyAndOversizedBatches) {
  BeaconObservation obs[BEACON_REPORT_MAX_ENTRIES + 1];
  memset(obs, 0, sizeof(obs));
  uint8_t buf[MAX_GROUP_DATA_LENGTH + BEACON_REPORT_ENTRY_LEN];
  EXPECT_EQ(0, beaconEncodeReport(buf, repeater_key, obs, 0));
  EXPECT_EQ(0, beaconEncodeReport(buf, repeater_key, obs, BEACON_REPORT_MAX_ENTRIES + 1));
  EXPECT_GT(beaconEncodeReport(buf, repeater_key, obs, BEACON_REPORT_MAX_ENTRIES), 0);
}

TEST_F(BeaconReportTest, BatchFillsClearsAndEncodes) {
  BeaconReportBatch batch;
  BeaconObservation o;
  memset(&o, 0, sizeof(o));
  EXPECT_EQ(0, batch.count());
  for (int i = 0; i < BEACON_REPORT_MAX_ENTRIES; i++) {
    EXPECT_FALSE(batch.isFull());
    o.counter = i;
    EXPECT_TRUE(batch.add(o));
  }
  EXPECT_TRUE(batch.isFull());
  EXPECT_FALSE(batch.add(o));
  uint8_t buf[MAX_GROUP_DATA_LENGTH];
  EXPECT_EQ(BEACON_REPORT_HEADER_LEN + BEACON_REPORT_MAX_ENTRIES * BEACON_REPORT_ENTRY_LEN, batch.encode(buf, repeater_key));
  batch.clear();
  EXPECT_EQ(0, batch.count());
  EXPECT_EQ(0, batch.encode(buf, repeater_key));   // nothing to send
}

TEST_F(BeaconReportTest, ClampInt8RoundsHalfAwayFromZeroAndSaturates) {
  EXPECT_EQ(-97, beaconClampInt8(-97.4f));
  EXPECT_EQ(-98, beaconClampInt8(-97.5f));
  EXPECT_EQ(1, beaconClampInt8(0.5f));
  EXPECT_EQ(-1, beaconClampInt8(-0.5f));
  EXPECT_EQ(0, beaconClampInt8(0.0f));
  EXPECT_EQ(127, beaconClampInt8(127.4f));
  EXPECT_EQ(127, beaconClampInt8(300.0f));
  EXPECT_EQ(-128, beaconClampInt8(-128.4f));
  EXPECT_EQ(-128, beaconClampInt8(-300.0f));
}

// Not a check: writes the vectors for the base repository when BEACON_REPORT_VECTORS_OUT is set.
TEST_F(BeaconReportTest, WritesVectorFileWhenRequested) {
  const char* path = getenv("BEACON_REPORT_VECTORS_OUT");
  if (path == NULL || *path == 0) GTEST_SKIP() << "set BEACON_REPORT_VECTORS_OUT to write the JSON vectors";

  FILE* f = fopen(path, "w");
  ASSERT_NE(nullptr, f) << path;

  fprintf(f, "{\n  \"format\": \"beacon_report\",\n  \"version\": %d,\n", BEACON_REPORT_VERSION);
  fprintf(f, "  \"constants\": {\n    \"data_type\": %d,\n    \"id_len\": %d,\n    \"header_len\": %d,\n"
             "    \"entry_len\": %d,\n    \"max_entries\": %d,\n    \"max_group_data_length\": %d\n  },\n",
          BEACON_REPORT_DATA_TYPE, BEACON_REPORT_ID_LEN, BEACON_REPORT_HEADER_LEN, BEACON_REPORT_ENTRY_LEN,
          BEACON_REPORT_MAX_ENTRIES, MAX_GROUP_DATA_LENGTH);
  fprintf(f, "  \"repeater_key\": \"%s\",\n", toHex(repeater_key, PUB_KEY_SIZE).c_str());
  fprintf(f, "  \"repeater_id\": \"%s\",\n", toHex(repeater_key, BEACON_REPORT_ID_LEN).c_str());

  fprintf(f, "  \"reports\": [\n");
  auto valid = validCases();
  for (size_t i = 0; i < valid.size(); i++) {
    fprintf(f, "    {\"name\": \"%s\", \"hex\": \"%s\", \"observations\": [\n", valid[i].name, valid[i].hex);
    for (size_t j = 0; j < valid[i].obs.size(); j++) {
      const Obs& o = valid[i].obs[j];
      fprintf(f, "      {\"beacon_id\": \"%s\", \"counter\": %lu, \"rssi\": %d, \"snr\": %d, \"batt_mv\": %u}%s\n",
              toHex(o.id, BEACON_REPORT_ID_LEN).c_str(), (unsigned long)o.counter, o.rssi, o.snr, (unsigned)o.batt_mv,
              j + 1 < valid[i].obs.size() ? "," : "");
    }
    fprintf(f, "    ]}%s\n", i + 1 < valid.size() ? "," : "");
  }
  fprintf(f, "  ],\n");

  // expect_count -1 means the decoder must reject the input; otherwise it decodes to that many entries
  fprintf(f, "  \"decode_only\": [\n");
  auto dec = decodeCases();
  for (size_t i = 0; i < dec.size(); i++) {
    fprintf(f, "    {\"name\": \"%s\", \"hex\": \"%s\", \"expect_count\": %d}%s\n", dec[i].name, dec[i].hex,
            dec[i].expect_count, i + 1 < dec.size() ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
