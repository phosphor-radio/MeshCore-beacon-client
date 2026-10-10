#include <gtest/gtest.h>

#include <string.h>

#include <helpers/BeaconAdvert.h>

TEST(BeaconDefaultName, DerivedFromTheFirstThreeKeyBytes) {
  uint8_t key[PUB_KEY_SIZE] = {0xf5, 0xb1, 0x65, 0x99, 0x88};
  char name[BEACON_DEFAULT_NAME_LEN + 1];
  beaconDefaultName(name, key);
  EXPECT_STREQ("beacon-f5b165", name);
}

TEST(BeaconDefaultName, PadsSingleDigitBytesAndUsesLowercase) {
  uint8_t key[PUB_KEY_SIZE] = {0x00, 0x0a, 0xAB};
  char name[BEACON_DEFAULT_NAME_LEN + 1];
  beaconDefaultName(name, key);
  EXPECT_STREQ("beacon-000aab", name);
}

TEST(BeaconDefaultName, IsUniquePerKeyAndIgnoresLaterBytes) {
  uint8_t a[PUB_KEY_SIZE] = {1, 2, 3, 4};
  uint8_t b[PUB_KEY_SIZE] = {1, 2, 4, 4};
  uint8_t c[PUB_KEY_SIZE] = {1, 2, 3, 9};
  char na[BEACON_DEFAULT_NAME_LEN + 1], nb[BEACON_DEFAULT_NAME_LEN + 1], nc[BEACON_DEFAULT_NAME_LEN + 1];
  beaconDefaultName(na, a); beaconDefaultName(nb, b); beaconDefaultName(nc, c);
  EXPECT_STRNE(na, nb);
  EXPECT_STREQ(na, nc);   // only the first three bytes count, the same six digits the base uses to abbreviate a prefix
}

TEST(BeaconDefaultName, FitsTheAdvertNameLimitAndIsNulTerminated) {
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0xFF, sizeof(key));
  char name[BEACON_DEFAULT_NAME_LEN + 1];
  memset(name, 'X', sizeof(name));
  beaconDefaultName(name, key);
  EXPECT_EQ((size_t)BEACON_DEFAULT_NAME_LEN, strlen(name));
  EXPECT_LE(strlen(name), (size_t)BEACON_MAX_NAME_LEN);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
