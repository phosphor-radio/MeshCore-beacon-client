#include <gtest/gtest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include <helpers/BeaconNames.h>

/*
 * Golden vectors for the beacon name announcement format (src/helpers/BeaconNames.h), plus the batch and the
 * announce-cache logic.
 *
 * The hex strings were derived with an independent encoder (Python) from the layout in BeaconNames.h, and the
 * plan_example case matches the example in the base station design document.  The base station (separate repository)
 * consumes the same vectors.  Set BEACON_NAMES_VECTORS_OUT to a file path to have this test write them as JSON:
 *
 *   BEACON_NAMES_VECTORS_OUT=/path/beacon_names_v1.json pio test -e native -f test_beacon_names
 */

namespace {

static uint8_t repeater_key[PUB_KEY_SIZE];   // bytes 0x00..0x1F, only the first 8 are transmitted

struct NameObs { std::vector<uint8_t> id; std::string name; };

struct ValidCase {
  const char* name;
  std::vector<NameObs> entries;
  const char* hex;
};

static NameObs make(std::initializer_list<uint8_t> id, const char* name) { return {std::vector<uint8_t>(id), name}; }

static std::vector<ValidCase> validCases() {
  std::vector<ValidCase> v;
  v.push_back({"plan_example",
    {make({0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7}, "beacon-001"), make({0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}, "Roof")},
    "01000102030405060702a0a1a2a3a4a5a6a70a626561636f6e2d303031112233445566778804526f6f66"});
  v.push_back({"default_name",
    {make({0xf5,0xb1,0x65,1,2,3,4,5}, "beacon-f5b165")},
    "01000102030405060701f5b16501020304050d626561636f6e2d663562313635"});
  v.push_back({"utf8_name",
    {make({0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27}, "Caf\xc3\xa9 \xf0\x9f\x94\x8b")},
    "0100010203040506070120212223242526270a436166c3a920f09f948b"});
  v.push_back({"max_beacon_name_18",
    {make({0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47}, "abcdefghijklmnopqr")},
    "010001020304050607014041424344454647126162636465666768696a6b6c6d6e6f707172"});

  // five 18-byte names and one 11-byte name use all 165 data bytes
  ValidCase full = {"fills_packet", {}, ""};
  full.hex =
    "0100010203040506070630313233343536371261616161616161616161616161616161616138393a3b3c3d3e3f1262626262626262626262626262"
    "626262626240414243444546471263636363636363636363636363636363636348494a4b4c4d4e4f12646464646464646464646464646464646464"
    "50515253545556571265656565656565656565656565656565656558595a5b5c5d5e5f0b7a7a7a7a7a7a7a7a7a7a7a";
  for (int i = 0; i < 6; i++) {
    NameObs e;
    for (int j = 0; j < 8; j++) e.id.push_back((uint8_t)(0x30 + i * 8 + j));
    e.name = std::string(i < 5 ? 18 : 11, (char)(i < 5 ? 'a' + i : 'z'));
    full.entries.push_back(e);
  }
  v.push_back(full);
  return v;
}

struct DecodeCase {
  const char* name;
  const char* hex;
  int expect_count;                          // -1 means the decoder must reject the message
  std::vector<NameObs> entries;              // expected output when expect_count >= 0
};

static std::vector<DecodeCase> decodeCases() {
  std::vector<NameObs> plan_entries = {
    make({0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7}, "beacon-001"), make({0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}, "Roof")};
  return {
    {"reject_empty", "", -1, {}},
    {"reject_short_header", "010001020304050607", -1, {}},
    {"reject_version_0", "00000102030405060702a0a1a2a3a4a5a6a70a626561636f6e2d303031112233445566778804526f6f66", -1, {}},
    {"reject_version_2", "02000102030405060702a0a1a2a3a4a5a6a70a626561636f6e2d303031112233445566778804526f6f66", -1, {}},
    {"reject_version_255", "ff000102030405060702a0a1a2a3a4a5a6a70a626561636f6e2d303031112233445566778804526f6f66", -1, {}},
    {"reject_entry_header_truncated", "010001020304050607010001020304", -1, {}},
    {"reject_name_runs_past_end", "01000102030405060701a0a1a2a3a4a5a6a70a62656163", -1, {}},
    {"reject_count_exceeds_entries", "01000102030405060702f5b16501020304050d626561636f6e2d663562313635", -1, {}},
    {"accept_empty_batch", "01000102030405060700", 0, {}},
    {"accept_trailing_bytes",
     "01000102030405060702a0a1a2a3a4a5a6a70a626561636f6e2d303031112233445566778804526f6f660000", 2, plan_entries},
    {"accept_zero_length_name_skipped",
     "01000102030405060702000000000000000000112233445566778804526f6f66", 1,
     {make({0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}, "Roof")}},
    {"accept_all_zero_length_names", "01000102030405060702000000000000000000000102030405060700", 0, {}},
    {"accept_name_longer_than_32",
     "0100010203040506070150515253545556572878787878787878787878787878787878787878787878787878787878787878787878787878787878", 1,
     {make({0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57}, std::string(40, 'x').c_str())}},
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

class BeaconNamesTest : public ::testing::Test {
protected:
  void SetUp() override { for (int i = 0; i < PUB_KEY_SIZE; i++) repeater_key[i] = (uint8_t)i; }
};

}  // namespace

TEST_F(BeaconNamesTest, ConstantsMatchTheDocumentedLayout) {
  EXPECT_EQ(0xFFBF, BEACON_NAMES_DATA_TYPE);
  EXPECT_EQ(1, BEACON_NAMES_VERSION);
  EXPECT_EQ(8, BEACON_NAMES_ID_LEN);
  EXPECT_EQ(10, BEACON_NAMES_HEADER_LEN);
  EXPECT_EQ(9, BEACON_NAMES_ENTRY_OVERHEAD);
  EXPECT_EQ(17, BEACON_NAMES_MAX_ENTRIES);
}

TEST_F(BeaconNamesTest, EncodesGoldenVectors) {
  for (const auto& c : validCases()) {
    BeaconNameBatch batch;
    for (const auto& e : c.entries) ASSERT_TRUE(batch.add(e.id.data(), e.name.c_str(), (int)e.name.size())) << c.name;
    uint8_t buf[MAX_GROUP_DATA_LENGTH];
    int n = batch.encode(buf, repeater_key);
    EXPECT_EQ(c.hex, toHex(buf, n)) << c.name;
  }
}

TEST_F(BeaconNamesTest, DecodesGoldenVectors) {
  for (const auto& c : validCases()) {
    std::vector<uint8_t> data = fromHex(c.hex);
    uint8_t rid[BEACON_NAMES_ID_LEN];
    BeaconNameEntry out[BEACON_NAMES_MAX_ENTRIES];
    int n = beaconNamesDecode(data.data(), (int)data.size(), rid, out, BEACON_NAMES_MAX_ENTRIES);
    ASSERT_EQ((int)c.entries.size(), n) << c.name;
    EXPECT_EQ(0, memcmp(rid, repeater_key, BEACON_NAMES_ID_LEN)) << c.name;
    for (int i = 0; i < n; i++) {
      EXPECT_EQ(0, memcmp(out[i].id, c.entries[i].id.data(), BEACON_NAMES_ID_LEN)) << c.name << " #" << i;
      EXPECT_EQ(c.entries[i].name, std::string((const char*)out[i].name, out[i].name_len)) << c.name << " #" << i;
    }
  }
}

TEST_F(BeaconNamesTest, DecodesAndRejectsDecodeOnlyCases) {
  for (const auto& c : decodeCases()) {
    std::vector<uint8_t> data = fromHex(c.hex);
    uint8_t rid[BEACON_NAMES_ID_LEN];
    BeaconNameEntry out[BEACON_NAMES_MAX_ENTRIES];
    int n = beaconNamesDecode(data.data(), (int)data.size(), rid, out, BEACON_NAMES_MAX_ENTRIES);
    ASSERT_EQ(c.expect_count, n) << c.name;
    for (int i = 0; i < n; i++) {
      EXPECT_EQ(0, memcmp(out[i].id, c.entries[i].id.data(), BEACON_NAMES_ID_LEN)) << c.name << " #" << i;
      EXPECT_EQ(c.entries[i].name, std::string((const char*)out[i].name, out[i].name_len)) << c.name << " #" << i;
    }
  }
}

TEST_F(BeaconNamesTest, DecoderHonoursCallerCapacity) {
  std::vector<uint8_t> data = fromHex(validCases()[0].hex);   // two entries
  uint8_t rid[BEACON_NAMES_ID_LEN];
  BeaconNameEntry out[1];
  EXPECT_EQ(-1, beaconNamesDecode(data.data(), (int)data.size(), rid, out, 1));
}

TEST_F(BeaconNamesTest, BatchRefusesWhatDoesNotFitAndLeavesItUnchanged) {
  BeaconNameBatch batch;
  uint8_t id[BEACON_NAMES_ID_LEN] = {0};
  uint8_t buf[MAX_GROUP_DATA_LENGTH];
  for (int i = 0; i < 5; i++) ASSERT_TRUE(batch.add(id, "abcdefghijklmnopqr", 18));
  ASSERT_TRUE(batch.add(id, "zzzzzzzzzzz", 11));                 // 20 bytes left, exactly this entry
  EXPECT_EQ(MAX_GROUP_DATA_LENGTH, batch.encode(buf, repeater_key));
  EXPECT_FALSE(batch.add(id, "x", 1));
  EXPECT_EQ(6, batch.count());
  EXPECT_EQ(MAX_GROUP_DATA_LENGTH, batch.encode(buf, repeater_key));

  batch.clear();
  EXPECT_EQ(0, batch.count());
  EXPECT_EQ(0, batch.encode(buf, repeater_key));                 // nothing to send
  EXPECT_FALSE(batch.add(id, "", 0));                            // empty names are never sent
  EXPECT_TRUE(batch.add(id, "x", 1));
}

TEST_F(BeaconNamesTest, NameHashIsFnv1a) {
  EXPECT_EQ(0x811c9dc5u, beaconNameHash("", 0));
  EXPECT_EQ(0xe40c292cu, beaconNameHash("a", 1));
  EXPECT_EQ(0xbf9cf968u, beaconNameHash("foobar", 6));
  EXPECT_NE(beaconNameHash("Roof", 4), beaconNameHash("roof", 4));
}

// ---- announce cache ----

class BeaconNameCacheTest : public ::testing::Test {
protected:
  static const uint32_t HOUR = 3600;
  BeaconNameCacheT<4> cache;
  uint8_t idA[8] = {1,1,1,1,1,1,1,1}, idB[8] = {2,2,2,2,2,2,2,2};
};

TEST_F(BeaconNameCacheTest, AnnouncesOnFirstSight) {
  EXPECT_EQ(BeaconNameCacheT<4>::FIRST_SEEN, cache.check(idA, 111, 0, 4 * HOUR));
  EXPECT_EQ(0, cache.size());                         // checking alone records nothing
  EXPECT_EQ(BeaconNameCacheT<4>::FIRST_SEEN, cache.check(idA, 111, 5, 4 * HOUR));
}

TEST_F(BeaconNameCacheTest, QuietUntilNameChangesOrRefreshIsDue) {
  cache.markAnnounced(idA, 111, 1000);
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(idA, 111, 1000, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(idA, 111, 1000 + 4 * HOUR - 1, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::REFRESH, cache.check(idA, 111, 1000 + 4 * HOUR, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::CHANGED, cache.check(idA, 222, 1001, 4 * HOUR));
}

TEST_F(BeaconNameCacheTest, RefreshRestartsFromEachAnnouncement) {
  cache.markAnnounced(idA, 111, 1000);
  cache.markAnnounced(idA, 111, 1000 + 4 * HOUR);
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(idA, 111, 1000 + 8 * HOUR - 1, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::REFRESH, cache.check(idA, 111, 1000 + 8 * HOUR, 4 * HOUR));
}

TEST_F(BeaconNameCacheTest, ZeroRefreshOnlyAnnouncesFirstSightAndChanges) {
  cache.markAnnounced(idA, 111, 0);
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(idA, 111, 100 * 24 * HOUR, 0));
  EXPECT_EQ(BeaconNameCacheT<4>::CHANGED, cache.check(idA, 222, 100 * 24 * HOUR, 0));
}

TEST_F(BeaconNameCacheTest, NameThatWasNotQueuedIsStillDue) {
  EXPECT_EQ(BeaconNameCacheT<4>::FIRST_SEEN, cache.check(idA, 111, 0, 4 * HOUR));
  // the caller could not queue it and does not call markAnnounced(): the next sighting asks again
  EXPECT_EQ(BeaconNameCacheT<4>::FIRST_SEEN, cache.check(idA, 111, 300, 4 * HOUR));
}

TEST_F(BeaconNameCacheTest, EvictsTheLeastRecentlyUsedEntry) {
  uint8_t ids[5][8];
  for (int i = 0; i < 5; i++) memset(ids[i], 0x10 + i, 8);
  for (int i = 0; i < 4; i++) cache.markAnnounced(ids[i], 100 + i, 10);
  EXPECT_EQ(4, cache.size());
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(ids[0], 100, 11, 4 * HOUR));   // touch 0, so 1 is now the oldest use
  cache.markAnnounced(ids[4], 104, 12);                                            // full: evicts ids[1]
  EXPECT_EQ(4, cache.size());
  EXPECT_EQ(BeaconNameCacheT<4>::FIRST_SEEN, cache.check(ids[1], 101, 13, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(ids[0], 100, 13, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(ids[2], 102, 13, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(ids[3], 103, 13, 4 * HOUR));
  EXPECT_EQ(BeaconNameCacheT<4>::NONE, cache.check(ids[4], 104, 13, 4 * HOUR));
}

TEST_F(BeaconNameCacheTest, ClearForgetsEverything) {
  cache.markAnnounced(idA, 111, 0);
  cache.markAnnounced(idB, 222, 0);
  cache.clear();
  EXPECT_EQ(0, cache.size());
  EXPECT_EQ(BeaconNameCacheT<4>::FIRST_SEEN, cache.check(idA, 111, 1, 4 * HOUR));
}

// Not a check: writes the vectors for the base repository when BEACON_NAMES_VECTORS_OUT is set.
TEST_F(BeaconNamesTest, WritesVectorFileWhenRequested) {
  const char* path = getenv("BEACON_NAMES_VECTORS_OUT");
  if (path == NULL || *path == 0) GTEST_SKIP() << "set BEACON_NAMES_VECTORS_OUT to write the JSON vectors";

  FILE* f = fopen(path, "w");
  ASSERT_NE(nullptr, f) << path;

  // names in the vectors are valid UTF-8 without quotes, backslashes or control characters, so they are JSON safe as is
  auto entriesJson = [&](const std::vector<NameObs>& entries, const char* indent) {
    for (size_t j = 0; j < entries.size(); j++) {
      const NameObs& e = entries[j];
      fprintf(f, "%s{\"beacon_id\": \"%s\", \"name\": \"%s\", \"name_hex\": \"%s\"}%s\n", indent,
              toHex(e.id.data(), e.id.size()).c_str(), e.name.c_str(),
              toHex((const uint8_t*)e.name.data(), e.name.size()).c_str(), j + 1 < entries.size() ? "," : "");
    }
  };

  fprintf(f, "{\n  \"format\": \"beacon_names\",\n  \"version\": %d,\n", BEACON_NAMES_VERSION);
  fprintf(f, "  \"constants\": {\n    \"data_type\": %d,\n    \"id_len\": %d,\n    \"header_len\": %d,\n"
             "    \"entry_overhead\": %d,\n    \"max_group_data_length\": %d\n  },\n",
          BEACON_NAMES_DATA_TYPE, BEACON_NAMES_ID_LEN, BEACON_NAMES_HEADER_LEN, BEACON_NAMES_ENTRY_OVERHEAD,
          MAX_GROUP_DATA_LENGTH);
  fprintf(f, "  \"repeater_key\": \"%s\",\n", toHex(repeater_key, PUB_KEY_SIZE).c_str());
  fprintf(f, "  \"repeater_id\": \"%s\",\n", toHex(repeater_key, BEACON_NAMES_ID_LEN).c_str());

  fprintf(f, "  \"messages\": [\n");
  auto valid = validCases();
  for (size_t i = 0; i < valid.size(); i++) {
    fprintf(f, "    {\"name\": \"%s\", \"hex\": \"%s\", \"entries\": [\n", valid[i].name, valid[i].hex);
    entriesJson(valid[i].entries, "      ");
    fprintf(f, "    ]}%s\n", i + 1 < valid.size() ? "," : "");
  }
  fprintf(f, "  ],\n");

  // expect_count -1 means the decoder must reject the message; otherwise "entries" lists what it returns, in order,
  // with zero-length names skipped
  fprintf(f, "  \"decode_only\": [\n");
  auto dec = decodeCases();
  for (size_t i = 0; i < dec.size(); i++) {
    fprintf(f, "    {\"name\": \"%s\", \"hex\": \"%s\", \"expect_count\": %d", dec[i].name, dec[i].hex, dec[i].expect_count);
    if (dec[i].expect_count >= 0) {
      fprintf(f, ", \"entries\": [\n");
      entriesJson(dec[i].entries, "      ");
      fprintf(f, "    ]");
    }
    fprintf(f, "}%s\n", i + 1 < dec.size() ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
