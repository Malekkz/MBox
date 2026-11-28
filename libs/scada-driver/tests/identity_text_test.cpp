// The text of `sys.dev.<device>.identity` (driver interface contract 3.6,
// 11.1; decision D-147).

#include "scada/driver/identity_text.h"
#include "scada/driver/tag_value.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace scada::driver {
namespace {

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers): the texts are the test data

std::string joined(const std::vector<IdentityField>& fields) {
    std::array<char, TAG_STRING_MAX_BYTES> out{};
    const std::size_t length = join_identity(fields, out);
    return {out.data(), length};
}

IdentityField read(const TagValue& value) {
    return IdentityField{.read = true, .value = value};
}

TEST(IdentityText, BytesOutsideThePrintableRangeBecomeQuestionMarks) {
    std::array<char, 16> out{};
    const std::string_view raw(
        "A\x01"
        "B\x7F"
        "C\xC3\xA9",
        7);
    const std::size_t length = clean_identity_text(raw, out);
    EXPECT_EQ(std::string_view(out.data(), length), "A?B?C??");
    // A space and a tilde are kept: the range is inclusive.
    EXPECT_EQ(std::string_view(out.data(), clean_identity_text(" ~", out)),
              " ~");
}

TEST(IdentityText, EachValueIsShownInItsOwnWay) {
    EXPECT_EQ(joined({read(int64_value(-42))}), "-42");
    EXPECT_EQ(
        joined({read(uint64_value(std::numeric_limits<std::uint64_t>::max()))}),
        "18446744073709551615");
    EXPECT_EQ(joined({read(float64_value(0.1))}), "0.1");  // shortest exact
    EXPECT_EQ(joined({read(bool_value(true))}), "true");
    EXPECT_EQ(joined({read(string_value("MODEL-A\n"))}), "MODEL-A?");
}

TEST(IdentityText, ChecksAreJoinedAndOneNotReadIsEmpty) {
    EXPECT_EQ(joined({read(string_value("MODEL-A")), read(uint64_value(3)),
                      IdentityField{}}),
              "MODEL-A; 3; ");
    EXPECT_EQ(joined({}), "");
    EXPECT_EQ(joined({IdentityField{}}), "");
}

TEST(IdentityText, ALongValueIsCutToItsShareAndMarked) {
    // Two checks: (250 - 2) / 2 = 124 bytes each.
    const std::string long_text(300, 'x');
    const std::string text =
        joined({read(string_value(long_text)), read(uint64_value(7))});
    ASSERT_EQ(text.size(), 124 + 2 + 1);
    EXPECT_EQ(text.substr(0, 123), std::string(123, 'x'));
    EXPECT_EQ(text[123], IDENTITY_CUT);
    EXPECT_EQ(text.substr(124), "; 7");
    // One value of exactly its share is not cut.
    const std::string exact(250, 'y');
    EXPECT_EQ(joined({read(string_value(exact))}), exact);
    const std::string over(251, 'y');
    const std::string cut = joined({read(string_value(over))});
    ASSERT_EQ(cut.size(), 250U);
    EXPECT_EQ(cut.back(), IDENTITY_CUT);
}

TEST(IdentityText, ManyChecksNeverOverflowTheString) {
    // More separators than room: every field is empty, the text still fits.
    const std::vector<IdentityField> fields(200, read(uint64_value(1)));
    const std::string text = joined(fields);
    EXPECT_LE(text.size(), TAG_STRING_MAX_BYTES);
    EXPECT_EQ(text.find('1'), std::string::npos);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

}  // namespace
}  // namespace scada::driver
