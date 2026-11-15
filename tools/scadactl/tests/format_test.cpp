// Unit tests for how scadactl writes values, qualities, flags, times and
// text.

#include "scada/ctl/format.h"

#include "scada/tag/scada_tag.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace scada::ctl {
namespace {

constexpr std::int64_t SOME_MONO_NS = 123'456'789;

// U+202E in UTF-8, built from its bytes so that no literal of this file
// holds it.
std::string right_to_left_override() {
    return {'\xe2', '\x80', '\xae'};
}

// Times for the age and time tests, in ns.
constexpr std::int64_t AGE_12_MS = 12'345'678;
constexpr std::int64_t AGE_61_S = 61'000'000'000;
constexpr std::int64_t NEGATIVE_AGE = -5;
// 2026-11-15T10:20:30.123456789Z
constexpr std::int64_t SOME_TIME_NS = 1'794'738'030'123'456'789;

scada_value value_of(std::uint8_t type, std::uint64_t bits) {
    scada_value value{};
    value.type = type;
    value.quality = SCADA_QUALITY_GOOD;
    value.value = bits;
    value.update_mono_ns = SOME_MONO_NS;
    return value;
}

std::string text_of(const scada_value& value, std::string_view text = {}) {
    std::string out;
    append_value_text(out, value, text);
    return out;
}

std::string json_of(const scada_value& value, std::string_view text = {}) {
    std::string out;
    append_value_json(out, value, text);
    return out;
}

std::string safe(std::string_view bytes) {
    std::string out;
    append_terminal_safe(out, bytes);
    return out;
}

std::string json_string(std::string_view bytes) {
    std::string out;
    append_json_string(out, bytes);
    return out;
}

std::string flags(std::uint16_t bits) {
    std::string out;
    append_flag_names(out, bits, ",");
    return out;
}

TEST(Format, TypeAndQualityNames) {
    EXPECT_EQ(type_name(SCADA_TYPE_BOOL), "bool");
    EXPECT_EQ(type_name(SCADA_TYPE_INT64), "int64");
    EXPECT_EQ(type_name(SCADA_TYPE_UINT64), "uint64");
    EXPECT_EQ(type_name(SCADA_TYPE_FLOAT64), "float64");
    EXPECT_EQ(type_name(SCADA_TYPE_STRING), "string");
    EXPECT_EQ(type_name(0), "unknown");
    EXPECT_EQ(type_name(6), "unknown");
    EXPECT_EQ(quality_name(SCADA_QUALITY_GOOD), "GOOD");
    EXPECT_EQ(quality_name(SCADA_QUALITY_UNCERTAIN), "UNCERTAIN");
    EXPECT_EQ(quality_name(SCADA_QUALITY_BAD), "BAD");
    EXPECT_EQ(quality_name(3), "UNKNOWN");
}

TEST(Format, FlagNamesInBitOrderAndUnknownBitsInHex) {
    EXPECT_EQ(flags(0), "");
    EXPECT_EQ(flags(SCADA_FLAG_STALE), "STALE");
    EXPECT_EQ(
        flags(SCADA_FLAG_OWNER_DOWN | SCADA_FLAG_STALE | SCADA_FLAG_LOCAL_TIME),
        "STALE,OWNER_DOWN,LOCAL_TIME");
    EXPECT_EQ(flags(0x8000), "0x8000");
    EXPECT_EQ(flags(0x1000 | SCADA_FLAG_COMM_FAIL), "COMM_FAIL,0x1000");
}

TEST(Format, AGoodValueWithAQualifyingFlagIsMarked) {
    for (const std::uint16_t flag : std::initializer_list<std::uint16_t>{
             SCADA_FLAG_STALE, SCADA_FLAG_COMM_FAIL, SCADA_FLAG_OUT_OF_RANGE,
             SCADA_FLAG_SUBSTITUTED, SCADA_FLAG_NOT_INIT,
             SCADA_FLAG_TIME_UNCERTAIN, SCADA_FLAG_BLOCKED, SCADA_FLAG_TEST,
             SCADA_FLAG_OVERFLOW, 0x1000}) {
        EXPECT_EQ(mark_of(SCADA_QUALITY_GOOD, flag), Mark::ATTENTION) << flag;
        EXPECT_EQ(
            mark_of(SCADA_QUALITY_GOOD,
                    static_cast<std::uint16_t>(flag | SCADA_FLAG_LOCAL_TIME)),
            Mark::ATTENTION)
            << flag;
    }
}

TEST(Format, BadAndStaleAreMarked) {
    EXPECT_EQ(mark_of(SCADA_QUALITY_GOOD, 0), Mark::NONE);
    EXPECT_EQ(mark_of(SCADA_QUALITY_GOOD, SCADA_FLAG_LOCAL_TIME), Mark::NONE);
    EXPECT_EQ(mark_of(SCADA_QUALITY_GOOD,
                      SCADA_FLAG_RESTORED | SCADA_FLAG_LOCAL_TIME),
              Mark::NONE);
    EXPECT_EQ(mark_of(SCADA_QUALITY_UNCERTAIN, 0), Mark::ATTENTION);
    EXPECT_EQ(mark_of(SCADA_QUALITY_UNCERTAIN, SCADA_FLAG_STALE),
              Mark::ATTENTION);
    EXPECT_EQ(mark_of(SCADA_QUALITY_BAD, 0), Mark::BAD);
    EXPECT_EQ(mark_of(SCADA_QUALITY_BAD, SCADA_FLAG_STALE), Mark::BAD);
    EXPECT_EQ(mark_of(SCADA_QUALITY_BAD, SCADA_FLAG_OWNER_DOWN), Mark::BAD);
    EXPECT_EQ(mark_of(7, 0), Mark::BAD);
    EXPECT_EQ(mark_char(Mark::NONE), ' ');
    EXPECT_EQ(mark_char(Mark::ATTENTION), '~');
    EXPECT_EQ(mark_char(Mark::BAD), '!');
}

TEST(Format, ValuesOfEachType) {
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_BOOL, 1)), "true");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_BOOL, 0)), "false");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_INT64, std::bit_cast<std::uint64_t>(
                                                     std::int64_t{-42}))),
              "-42");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_UINT64,
                               std::numeric_limits<std::uint64_t>::max())),
              "18446744073709551615");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(0.1))),
              "0.1");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(-1.5e300))),
              "-1.5e+300");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_STRING, 0), "on"), "\"on\"");
    EXPECT_EQ(json_of(value_of(SCADA_TYPE_BOOL, 1)), "true");
    EXPECT_EQ(json_of(value_of(SCADA_TYPE_INT64, std::bit_cast<std::uint64_t>(
                                                     std::int64_t{-42}))),
              "-42");
    EXPECT_EQ(json_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(2.5))),
              "2.5");
    EXPECT_EQ(json_of(value_of(SCADA_TYPE_STRING, 0), "a\"b"), "\"a\\\"b\"");
}

TEST(Format, DoublesThatAreNotFinite) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(nan))),
              "nan");
    EXPECT_EQ(text_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(-inf))),
              "-inf");
    EXPECT_EQ(json_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(nan))),
              "\"nan\"");
    EXPECT_EQ(json_of(value_of(SCADA_TYPE_FLOAT64,
                               std::bit_cast<std::uint64_t>(inf))),
              "\"inf\"");
}

TEST(Format, NoValueWithoutAWrite) {
    scada_value never{};
    never.type = SCADA_TYPE_FLOAT64;
    never.quality = SCADA_QUALITY_BAD;
    never.quality_flags = SCADA_FLAG_NOT_INIT;
    EXPECT_FALSE(has_value(never));
    EXPECT_EQ(text_of(never), "-");
    EXPECT_EQ(json_of(never), "null");
    // A type that is no type has no value to show either.
    EXPECT_EQ(text_of(value_of(9, 1)), "-");
    EXPECT_EQ(json_of(value_of(9, 1)), "null");
}

TEST(Format, TerminalSafeText) {
    EXPECT_EQ(safe("plain text 1.5"), "plain text 1.5");
    EXPECT_EQ(safe("a\"b\\c"), "a\\\"b\\\\c");
    EXPECT_EQ(safe("l1\nl2\r\t"), "l1\\nl2\\r\\t");
    // An escape sequence that would clear the screen.
    EXPECT_EQ(safe("\x1b[2J"), "\\x1b[2J");
    EXPECT_EQ(safe(std::string_view("\0\x7f", 2)), "\\x00\\x7f");
    // Well-formed UTF-8 stays, a C1 control and a direction override do
    // not, and neither does a byte that is not UTF-8.
    EXPECT_EQ(safe("Druck \xc2\xb0"
                   "C"),
              "Druck \xc2\xb0"
              "C");
    EXPECT_EQ(safe("\xc2\x9b"
                   "31m"),
              "\\u009b31m");
    EXPECT_EQ(safe("a" + right_to_left_override() + "b"), "a\\u202eb");
    EXPECT_EQ(safe("\xff\xc0\xaf"), "\\xff\\xc0\\xaf");
    // A surrogate and an overlong form are not well-formed.
    EXPECT_EQ(safe("\xed\xa0\x80"), "\\xed\\xa0\\x80");
    EXPECT_EQ(safe("\xe0\x80\xaf"), "\\xe0\\x80\\xaf");
    // A sequence cut short at the end.
    EXPECT_EQ(safe("ok\xe2\x82"), "ok\\xe2\\x82");
}

TEST(Format, JsonStrings) {
    EXPECT_EQ(json_string(""), "\"\"");
    EXPECT_EQ(json_string("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(json_string("\n\x01\x7f"), "\"\\n\\u0001\\u007f\"");
    EXPECT_EQ(json_string("\xc2\xb0"), "\"\xc2\xb0\"");
    EXPECT_EQ(json_string("\xff"
                          "a"),
              "\"\\ufffda\"");
    EXPECT_EQ(json_string(right_to_left_override()), "\"\\u202e\"");
}

TEST(Format, Ages) {
    std::string out;
    append_age(out, 0);
    EXPECT_EQ(out, "0.000 s");
    out.clear();
    append_age(out, AGE_12_MS);
    EXPECT_EQ(out, "0.012 s");
    out.clear();
    append_age(out, AGE_61_S);
    EXPECT_EQ(out, "61.000 s");
    out.clear();
    append_age(out, NEGATIVE_AGE);
    EXPECT_EQ(out, "0.000 s");
}

TEST(Format, UtcTimes) {
    std::string out;
    append_utc_time(out, 0);
    EXPECT_EQ(out, "-");
    out.clear();
    append_utc_time(out, SOME_TIME_NS);
    EXPECT_EQ(out, "2026-11-15T10:20:30.123Z");
    out.clear();
    append_utc_time(out, 1);
    EXPECT_EQ(out, "1970-01-01T00:00:00.000Z");
}

}  // namespace
}  // namespace scada::ctl
