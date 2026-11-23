// Typed reads of a scalar node (read.h): the forms of contract 1.2 and 2.2,
// and the range of each type.

#include "scada/conf/read.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {
namespace {

// The document `v: <text>`; a failed parse is a test failure and gives a
// document whose value is an empty map.
class OneValue {
public:
    explicit OneValue(std::string_view text)
        : document_(parse("v: " + std::string(text) + "\n")) {}

    [[nodiscard]] const Document& document() const { return document_; }
    [[nodiscard]] NodeId value() const {
        return document_.entries(document_.root())[0].value;
    }

private:
    static Document parse(const std::string& text) {
        Diagnostics diagnostics;
        std::optional<Document> parsed =
            parse_yaml(text, "f.yaml", diagnostics);
        if (parsed.has_value()) {
            return std::move(*parsed);
        }
        ADD_FAILURE() << "no document for " << text;
        DocumentBuilder empty;
        static_cast<void>(empty.add_file("f.yaml"));
        static_cast<void>(empty.begin_map(SourcePosition{}));
        static_cast<void>(empty.key("v", ScalarStyle::PLAIN, SourcePosition{}));
        static_cast<void>(empty.begin_map(SourcePosition{}));
        static_cast<void>(empty.end_map());
        static_cast<void>(empty.end_map());
        return std::move(empty.finish()).value();
    }

    Document document_;
};

template <typename T>
StatusCode code_of(const Result<T>& result) {
    return result.status().code();
}

TEST(Read, Int64) {
    const OneValue negative("-12");
    EXPECT_EQ(to_int64(negative.document(), negative.value()).value(), -12);
    const OneValue hex("0x10");
    EXPECT_EQ(to_int64(hex.document(), hex.value()).value(), 16);
    const OneValue max("9223372036854775807");
    EXPECT_EQ(to_int64(max.document(), max.value()).value(), INT64_MAX);
    const OneValue over("9223372036854775808");
    EXPECT_EQ(code_of(to_int64(over.document(), over.value())),
              StatusCode::OUT_OF_RANGE);
    const OneValue quoted("'12'");
    EXPECT_EQ(code_of(to_int64(quoted.document(), quoted.value())),
              StatusCode::INVALID_ARGUMENT);
    const OneValue decimal("12.0");
    EXPECT_EQ(code_of(to_int64(decimal.document(), decimal.value())),
              StatusCode::INVALID_ARGUMENT);
    const OneValue map("{a: 1}");
    EXPECT_EQ(code_of(to_int64(map.document(), map.value())),
              StatusCode::INVALID_ARGUMENT);
}

TEST(Read, Uint64) {
    const OneValue max("18446744073709551615");
    EXPECT_EQ(to_uint64(max.document(), max.value()).value(), UINT64_MAX);
    const OneValue negative("-1");
    EXPECT_EQ(code_of(to_uint64(negative.document(), negative.value())),
              StatusCode::OUT_OF_RANGE);
}

TEST(Read, DecimalAndBool) {
    const OneValue decimal("2.5");
    EXPECT_EQ(to_decimal(decimal.document(), decimal.value()).value(), 2.5);
    const OneValue integer("-3");
    EXPECT_EQ(to_decimal(integer.document(), integer.value()).value(), -3.0);
    const OneValue word("high");
    EXPECT_EQ(code_of(to_decimal(word.document(), word.value())),
              StatusCode::INVALID_ARGUMENT);
    const OneValue yes("true");
    EXPECT_TRUE(to_bool(yes.document(), yes.value()).value());
    const OneValue quoted("'true'");
    EXPECT_EQ(code_of(to_bool(quoted.document(), quoted.value())),
              StatusCode::INVALID_ARGUMENT);
    const OneValue other("yes");
    EXPECT_EQ(code_of(to_bool(other.document(), other.value())),
              StatusCode::INVALID_ARGUMENT);
}

TEST(Read, DurationInNanoseconds) {
    constexpr std::int64_t MS_100 = 100000000;
    const OneValue plain("100ms");
    EXPECT_EQ(to_duration_ns(plain.document(), plain.value()).value(), MS_100);
    const OneValue quoted("'5s'");
    EXPECT_EQ(to_duration_ns(quoted.document(), quoted.value()).value(),
              std::int64_t{5} * 1000 * 1000 * 1000);
    const OneValue bad("5 s");
    EXPECT_EQ(code_of(to_duration_ns(bad.document(), bad.value())),
              StatusCode::INVALID_ARGUMENT);
    // 300 years do not fit int64 nanoseconds.
    const OneValue long_time("300y");
    EXPECT_EQ(code_of(to_duration_ns(long_time.document(), long_time.value())),
              StatusCode::OUT_OF_RANGE);
}

TEST(Read, RawPatternOfAWidth) {
    constexpr unsigned WIDTH_16 = 16;
    constexpr unsigned WIDTH_64 = 64;
    const OneValue minus_one("-1");
    EXPECT_EQ(to_raw_pattern(minus_one.document(), minus_one.value(), WIDTH_16)
                  .value(),
              0xFFFFU);
    EXPECT_EQ(to_raw_pattern(minus_one.document(), minus_one.value(), WIDTH_64)
                  .value(),
              UINT64_MAX);
    const OneValue top("65535");
    EXPECT_EQ(to_raw_pattern(top.document(), top.value(), WIDTH_16).value(),
              0xFFFFU);
    const OneValue over("65536");
    EXPECT_EQ(code_of(to_raw_pattern(over.document(), over.value(), WIDTH_16)),
              StatusCode::OUT_OF_RANGE);
    const OneValue most_negative("-32768");
    EXPECT_EQ(to_raw_pattern(most_negative.document(), most_negative.value(),
                             WIDTH_16)
                  .value(),
              0x8000U);
    const OneValue below("-32769");
    EXPECT_EQ(
        code_of(to_raw_pattern(below.document(), below.value(), WIDTH_16)),
        StatusCode::OUT_OF_RANGE);
    EXPECT_EQ(code_of(to_raw_pattern(top.document(), top.value(), 0)),
              StatusCode::INVALID_ARGUMENT);
}

TEST(Read, IntegerForm) {
    const OneValue ten("10");
    EXPECT_TRUE(is_integer_form(ten.document(), ten.value()));
    const OneValue hex("0x10");
    EXPECT_TRUE(is_integer_form(hex.document(), hex.value()));
    const OneValue decimal("10.0");
    EXPECT_FALSE(is_integer_form(decimal.document(), decimal.value()));
    const OneValue quoted("'10'");
    EXPECT_FALSE(is_integer_form(quoted.document(), quoted.value()));
}

}  // namespace
}  // namespace scada::conf
