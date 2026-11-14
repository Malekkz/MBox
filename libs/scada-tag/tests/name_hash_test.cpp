// Unit tests for the tag name hash (contract, section 5).

#include "name_hash.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using scada::tag::fnv1a_64;

// Usable in constant expressions, so a hash of a literal costs nothing.
static_assert(fnv1a_64("") == scada::tag::FNV1A_64_OFFSET_BASIS);

// Published test vectors of 64-bit FNV-1a.
TEST(NameHash, MatchesPublishedVectors) {
    EXPECT_EQ(fnv1a_64(""), 0xCBF2'9CE4'8422'2325U);
    EXPECT_EQ(fnv1a_64("a"), 0xAF63'DC4C'8601'EC8CU);
    EXPECT_EQ(fnv1a_64("b"), 0xAF63'DF4C'8601'F1A5U);
    EXPECT_EQ(fnv1a_64("foobar"), 0x8594'4171'F739'67E8U);
}

// A byte above 127 must enter the hash as 128 to 255, not sign-extended.
TEST(NameHash, HashesBytesAsUnsigned) {
    EXPECT_EQ(fnv1a_64("\xFF"), 0xAF64'724C'8602'EB6EU);
}

TEST(NameHash, HashesEmbeddedNulBytes) {
    const std::string_view with_nul("a\0b", 3);
    EXPECT_NE(fnv1a_64(with_nul), fnv1a_64("a"));
    EXPECT_NE(fnv1a_64(with_nul), fnv1a_64("ab"));
}

TEST(NameHash, DependsOnOrder) {
    EXPECT_NE(fnv1a_64("plant.pump1"), fnv1a_64("plant.pump2"));
    EXPECT_NE(fnv1a_64("ab"), fnv1a_64("ba"));
}

}  // namespace
