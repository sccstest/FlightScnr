#include <unity.h>

#include <cstring>

#include "services/notable_aircraft.h"

using services::notable::kMaxLabelLen;
using services::notable::kMaxTagLen;
using services::notable::match;
using services::notable::Match;
using services::notable::Rarity;

void setUp(void) {}
void tearDown(void) {}

static Match m(const char* type, const char* cs, const char* reg, const char* hex = "",
               bool mil = false, bool expect = true) {
  Match out;
  const bool hit = match(type, cs, reg, hex, mil, &out);
  TEST_ASSERT_EQUAL(expect, hit);
  return out;
}

void test_hurricane_hunters(void) {
  Match a = m("P3", "NOAA42", "N42RF");
  TEST_ASSERT_EQUAL_STRING("NOAA Hurricane Hunter", a.label);
  TEST_ASSERT_EQUAL_STRING("Hurricane Hunter", a.tag);
  TEST_ASSERT_TRUE(a.rarity == Rarity::VeryRare);
  TEST_ASSERT_EQUAL_STRING("NOAA Hurricane Hunter", m("GLF4", "", "N49RF").label);
  TEST_ASSERT_EQUAL_STRING("NOAA Hurricane Hunter", m("P3", "NOAA43", "").label);
  // Other NOAA airframes are still flagged, as plain research aircraft.
  Match twin = m("DHC6", "NOAA46", "N46RF");
  TEST_ASSERT_EQUAL_STRING("NOAA research aircraft", twin.label);
  TEST_ASSERT_TRUE(twin.rarity == Rarity::Notable);
  // Not NOAA: other N..RF shapes and a navy P-3 without NOAA markings.
  m("C172", "N123RF", "N123RF", "", false, false);
  m("P3", "", "N12RF", "", false, false);
  // Private N4#RF / N3#RF tails only count when the type is a WP-3D or G-IV.
  m("P46T", "N34RF", "N34RF", "", false, false);
  m("PA32", "N44RF", "N44RF", "", false, false);
}

void test_coast_guard(void) {
  TEST_ASSERT_EQUAL_STRING("Coast Guard HC-130J", m("C30J", "C2009", "").label);
  TEST_ASSERT_EQUAL_STRING("Coast Guard MH-60T", m("H60", "C6026", "").label);
  TEST_ASSERT_EQUAL_STRING("Coast Guard MH-60T", m("H60", "CG6026", "").label);
  TEST_ASSERT_EQUAL_STRING("Coast Guard aircraft", m("", "CG2311", "").label);
  // "C" + 4 digits on an unknown type only counts when flagged military.
  TEST_ASSERT_EQUAL_STRING("Coast Guard aircraft", m("", "C1234", "", "", true).label);
  m("", "C1234", "", "", false, false);
  // Five digits is not an airframe number.
  m("", "C12345", "", "", true, false);
}

void test_vip(void) {
  Match af1 = m("", "AF1", "");
  TEST_ASSERT_EQUAL_STRING("Air Force One", af1.label);
  TEST_ASSERT_TRUE(af1.rarity == Rarity::VeryRare);
  m("", "AF12", "", "", false, false);  // AF1 must be the whole callsign
  TEST_ASSERT_EQUAL_STRING("Special Air Mission", m("C32", "SAM44", "").label);
  m("", "SAMBA1", "", "", false, false);
  TEST_ASSERT_EQUAL_STRING("Presidential VC-25", m("VC25", "", "").label);
  TEST_ASSERT_EQUAL_STRING("Presidential VC-25", m("", "", "", "ADFDF8").label);
}

void test_military_heavies(void) {
  TEST_ASSERT_EQUAL_STRING("KC-135", m("K35R", "PACK41", "").tag);
  TEST_ASSERT_EQUAL_STRING("KC-46", m("KC46", "", "").tag);
  TEST_ASSERT_EQUAL_STRING("C-17", m("C17", "RCH123", "").tag);
  TEST_ASSERT_TRUE(m("C5M", "", "").rarity == Rarity::VeryRare);
  TEST_ASSERT_EQUAL_STRING("C-130", m("C130", "", "").tag);
  TEST_ASSERT_TRUE(m("B52", "", "").rarity == Rarity::VeryRare);
  // C-130 with a Coast Guard callsign is labelled Coast Guard, not plain C-130.
  TEST_ASSERT_EQUAL_STRING("Coast Guard", m("C130", "C2005", "").tag);
}

void test_big_civil(void) {
  TEST_ASSERT_EQUAL_STRING("747 Jumbo", m("B744", "GTI8811", "N464MC").tag);
  TEST_ASSERT_EQUAL_STRING("747 Jumbo", m("b748", "CLX792", "").tag);  // case-insensitive
  TEST_ASSERT_EQUAL_STRING("Dreamlifter", m("BLCF", "GTI4152", "").tag);
  TEST_ASSERT_TRUE(m("A388", "", "").rarity == Rarity::VeryRare);
  TEST_ASSERT_EQUAL_STRING("Beluga", m("A337", "", "").tag);
  TEST_ASSERT_EQUAL_STRING("An-124", m("A124", "", "").tag);
}

void test_ordinary_traffic(void) {
  m("B738", "SWA1234", "N8501V", "a1b2c3", false, false);
  m("A321", "AAL576", "", "", false, false);
  m("C172", "N12345", "N12345", "", false, false);
  m("", "", "", "", false, false);
  m(nullptr, nullptr, nullptr, nullptr, false, false);
  // Prefixes of real codes are not matches (B74 / C5 vs C510 etc.).
  m("B74", "", "", "", false, false);
  m("C510", "", "", "", false, false);
  m("B737", "", "", "", false, false);
}

void test_labels_fit(void) {
  for (size_t i = 0; i < services::notable::ruleCount(); ++i) {
    const Match r = services::notable::ruleAt(i);
    TEST_ASSERT_NOT_NULL(r.label);
    TEST_ASSERT_NOT_NULL(r.tag);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r.label) <= kMaxLabelLen, r.label);
    TEST_ASSERT_TRUE_MESSAGE(strlen(r.tag) <= kMaxTagLen, r.tag);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hurricane_hunters);
  RUN_TEST(test_coast_guard);
  RUN_TEST(test_vip);
  RUN_TEST(test_military_heavies);
  RUN_TEST(test_big_civil);
  RUN_TEST(test_ordinary_traffic);
  RUN_TEST(test_labels_fit);
  return UNITY_END();
}
