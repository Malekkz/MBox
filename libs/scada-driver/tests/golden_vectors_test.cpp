// Runs the shared golden vectors of tests/golden/driver-vectors.txt
// (driver interface contract 5.1: the vectors "are shared by #104, #105 and
// the simulators"; conformance test 5). The file's own header states the
// format. A line that does not parse fails the test, so that a vector is
// never skipped silently.

#include "sample_helpers.h"
#include "scada/driver/point_rules.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/sample_processing.h"
#include "scada/driver/tag_value.h"
#include "scada/driver/write_conversion.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace scada::driver {
namespace {

using tag::Quality;
using tag::QualityFlag;
using tag::TagType;

constexpr std::int64_t RECEIVE_NS = 1'800'000'000'000'000'000;
constexpr std::int64_t MONO_NS = 5'000'000'000;
constexpr int HEX = 16;
constexpr int DECIMAL = 10;

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : text) {
        if (character == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    parts.push_back(current);
    return parts;
}

template <typename T>
std::optional<T> integer(std::string_view text) {
    T value{};
    int base = DECIMAL;
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
        base = HEX;
    }
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> decimal(std::string_view text) {
    double value = 0.0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint16_t> flag_named(std::string_view name) {
    static const std::map<std::string_view, QualityFlag> NAMES{
        {"STALE", QualityFlag::STALE},
        {"COMM_FAIL", QualityFlag::COMM_FAIL},
        {"OUT_OF_RANGE", QualityFlag::OUT_OF_RANGE},
        {"SUBSTITUTED", QualityFlag::SUBSTITUTED},
        {"NOT_INIT", QualityFlag::NOT_INIT},
        {"OWNER_DOWN", QualityFlag::OWNER_DOWN},
        {"TIME_UNCERTAIN", QualityFlag::TIME_UNCERTAIN},
        {"BLOCKED", QualityFlag::BLOCKED},
        {"TEST", QualityFlag::TEST},
        {"OVERFLOW", QualityFlag::OVERFLOW},
        {"RESTORED", QualityFlag::RESTORED},
        {"LOCAL_TIME", QualityFlag::LOCAL_TIME}};
    const auto found = NAMES.find(name);
    if (found == NAMES.end()) {
        return std::nullopt;
    }
    return tag::to_raw(found->second);
}

std::optional<std::uint16_t> flags_of(std::string_view text) {
    if (text == "-") {
        return 0;
    }
    std::uint16_t out = 0;
    for (const std::string& name : split(text, ',')) {
        const std::optional<std::uint16_t> one = flag_named(name);
        if (!one.has_value()) {
            return std::nullopt;
        }
        out = static_cast<std::uint16_t>(out | *one);
    }
    return out;
}

std::optional<TagValue> value_of(std::string_view text) {
    const std::vector<std::string> parts = split(text, ':');
    if (parts.size() != 2) {
        return std::nullopt;
    }
    if (parts[0] == "int64") {
        const auto number = integer<std::int64_t>(parts[1]);
        return number.has_value() ? std::optional(int64_value(*number))
                                  : std::nullopt;
    }
    if (parts[0] == "uint64") {
        const auto number = integer<std::uint64_t>(parts[1]);
        return number.has_value() ? std::optional(uint64_value(*number))
                                  : std::nullopt;
    }
    if (parts[0] == "float64") {
        const auto bits = integer<std::uint64_t>(parts[1]);
        return bits.has_value()
                   ? std::optional(float64_value(std::bit_cast<double>(*bits)))
                   : std::nullopt;
    }
    if (parts[0] == "bool" && (parts[1] == "0" || parts[1] == "1")) {
        return bool_value(parts[1] == "1");
    }
    return std::nullopt;
}

std::optional<RawValue> raw_of(std::string_view text) {
    const std::vector<std::string> parts = split(text, ':');
    if (parts.size() == 2 && parts[0] == "float32") {
        const auto bits = integer<std::uint32_t>(parts[1]);
        return bits.has_value()
                   ? std::optional(raw_float32(std::bit_cast<float>(*bits)))
                   : std::nullopt;
    }
    if (parts.size() == 2 && parts[0] == "float64") {
        const auto bits = integer<std::uint64_t>(parts[1]);
        return bits.has_value()
                   ? std::optional(raw_float64(std::bit_cast<double>(*bits)))
                   : std::nullopt;
    }
    if (parts.size() == 2 && parts[0] == "bool" &&
        (parts[1] == "0" || parts[1] == "1")) {
        return raw_bool(parts[1] == "1");
    }
    if (parts.size() == 3) {
        const auto width = integer<unsigned>(parts[1]);
        if (!width.has_value()) {
            return std::nullopt;
        }
        if (parts[0] == "signed") {
            const auto number = integer<std::int64_t>(parts[2]);
            return number.has_value()
                       ? std::optional(raw_signed(*number, *width))
                       : std::nullopt;
        }
        if (parts[0] == "unsigned") {
            const auto number = integer<std::uint64_t>(parts[2]);
            return number.has_value()
                       ? std::optional(raw_unsigned(*number, *width))
                       : std::nullopt;
        }
    }
    return std::nullopt;
}

// The rules of one `rules` line.
struct NamedRules {
    PointRules rules;
};

// One `key=value` of a `rules` line that sets the scale; false when it does
// not parse.
bool scale_key(const std::string& key, const std::string& value,
               ScaleRule& scale) {
    if (key == "gain" || key == "offset") {
        const auto number = decimal(value);
        if (!number.has_value()) {
            return false;
        }
        (key == "gain" ? scale.gain : scale.offset) = *number;
        return true;
    }
    if (key == "gain_i" || key == "offset_i") {
        const auto number = integer<std::int64_t>(value);
        if (!number.has_value()) {
            return false;
        }
        (key == "gain_i" ? scale.gain_i : scale.offset_i) = *number;
        (key == "gain_i" ? scale.gain : scale.offset) =
            static_cast<double>(*number);
        scale.integer = true;
        return true;
    }
    const auto number = integer<std::uint64_t>(value);
    if (!number.has_value()) {
        return false;
    }
    scale.form = ScaleRule::Form::DIVIDE_BY;
    scale.divide_by = *number;
    return true;
}

std::optional<InvalidMarker> marker_of(const std::string& value) {
    const std::vector<std::string> parts = split(value, ':');
    InvalidMarker marker;
    if (parts[0] == "nan") {
        marker.any_nan = true;
    } else {
        const auto pattern = integer<std::uint64_t>(parts[0]);
        if (!pattern.has_value()) {
            return std::nullopt;
        }
        marker.pattern = *pattern;
    }
    for (std::size_t part = 1; part < parts.size(); ++part) {
        const auto one = flag_named(parts[part]);
        if (!one.has_value()) {
            return std::nullopt;
        }
        marker.flags = static_cast<std::uint16_t>(marker.flags | *one);
    }
    return marker;
}

std::optional<TagType> tag_type_named(const std::string& name) {
    static const std::map<std::string, TagType> TYPES{
        {"bool", TagType::BOOL},
        {"int64", TagType::INT64},
        {"uint64", TagType::UINT64},
        {"float64", TagType::FLOAT64}};
    const auto found = TYPES.find(name);
    return found == TYPES.end() ? std::nullopt : std::optional(found->second);
}

// One `key=value` of a `rules` line that is not a scale; false when it does
// not parse.
bool other_key(const std::string& key, const std::string& value,
               PointRules& rules, bool& has_tag) {
    if (key == "tag") {
        const auto type = tag_type_named(value);
        has_tag = type.has_value();
        rules.tag_type = type.value_or(TagType::INT64);
        return has_tag;
    }
    if (key == "marker") {
        const auto marker = marker_of(value);
        if (marker.has_value()) {
            rules.markers.push_back(*marker);
        }
        return marker.has_value();
    }
    if (key == "raw_range") {
        const std::vector<std::string> parts = split(value, ':');
        const auto min = parts.size() == 2 ? decimal(parts[0]) : std::nullopt;
        const auto max = parts.size() == 2 ? decimal(parts[1]) : std::nullopt;
        if (!min.has_value() || !max.has_value()) {
            return false;
        }
        rules.raw_range = RawRange{.min = *min, .max = *max};
        return true;
    }
    if (key == "enum") {
        rules.has_enum = true;
        for (const std::string& code : split(value, ',')) {
            const auto number = integer<std::int64_t>(code);
            if (!number.has_value()) {
                return false;
            }
            rules.enum_codes.push_back(*number);
        }
        return true;
    }
    if (key == "invert") {
        rules.invert = true;
        return true;
    }
    return false;
}

std::optional<NamedRules> rules_of(const std::vector<std::string>& fields) {
    NamedRules out;
    bool has_tag = false;
    ScaleRule scale;
    bool has_scale = false;
    for (std::size_t index = 2; index < fields.size(); ++index) {
        const std::string& field = fields[index];
        const std::size_t equals = field.find('=');
        const std::string key = field.substr(0, equals);
        const std::string value =
            equals == std::string::npos ? "" : field.substr(equals + 1);
        const bool is_scale = key == "gain" || key == "offset" ||
                              key == "gain_i" || key == "offset_i" ||
                              key == "divide_by";
        const bool parsed = is_scale
                                ? scale_key(key, value, scale)
                                : other_key(key, value, out.rules, has_tag);
        if (!parsed) {
            return std::nullopt;
        }
        has_scale = has_scale || is_scale;
    }
    if (has_scale) {
        out.rules.scale = scale;
    }
    if (!has_tag || !validate_point_rules(out.rules).ok()) {
        return std::nullopt;
    }
    return out;
}

// The fields after `->`, as key=value.
std::map<std::string, std::string> results_of(
    const std::vector<std::string>& fields, std::size_t arrow) {
    std::map<std::string, std::string> out;
    for (std::size_t index = arrow + 1; index < fields.size(); ++index) {
        const std::size_t equals = fields[index].find('=');
        out[fields[index].substr(0, equals)] =
            equals == std::string::npos ? "" : fields[index].substr(equals + 1);
    }
    return out;
}

std::string field_value(const std::vector<std::string>& fields, std::size_t end,
                        std::string_view key) {
    for (std::size_t index = 2; index < end; ++index) {
        if (fields[index].starts_with(std::string(key) + "=")) {
            return fields[index].substr(key.size() + 1);
        }
    }
    return {};
}

bool same(const TagValue& got, const TagValue& want) {
    if (got.type != want.type) {
        return false;
    }
    switch (want.type) {
        case TagType::BOOL:
            return got.b == want.b;
        case TagType::INT64:
            return got.i == want.i;
        case TagType::UINT64:
            return got.u == want.u;
        case TagType::FLOAT64:
            return std::bit_cast<std::uint64_t>(got.f) ==
                   std::bit_cast<std::uint64_t>(want.f);
        case TagType::STRING:
            return got.text == want.text;
    }
    return false;
}

std::optional<Quality> quality_named(std::string_view name) {
    if (name == "GOOD") {
        return Quality::GOOD;
    }
    if (name == "UNCERTAIN") {
        return Quality::UNCERTAIN;
    }
    if (name == "BAD") {
        return Quality::BAD;
    }
    return std::nullopt;
}

std::string result_field(const std::map<std::string, std::string>& results,
                         const std::string& key) {
    const auto found = results.find(key);
    return found == results.end() ? std::string() : found->second;
}

void run_sample(const PointRules& rules, const std::vector<std::string>& fields,
                std::size_t arrow, int line) {
    const std::optional<RawValue> raw =
        raw_of(field_value(fields, arrow, "raw"));
    const auto results = results_of(fields, arrow);
    const auto want_value = value_of(result_field(results, "value"));
    const auto want_quality = quality_named(result_field(results, "quality"));
    const auto want_flags = flags_of(result_field(results, "flags"));
    if (!raw.has_value() || !want_value.has_value() ||
        !want_quality.has_value() || !want_flags.has_value()) {
        ADD_FAILURE() << "line " << line << " does not parse";
        return;
    }
    PointState state = test::fresh_state(rules);
    const ProcessedSample out = test::processed(
        rules, state, RawSample{.outcome = Outcome::VALUE, .raw = *raw}, {},
        SampleTimes{.receive_time_ns = RECEIVE_NS, .receive_mono_ns = MONO_NS});
    EXPECT_TRUE(same(out.tag.value, *want_value)) << "line " << line;
    EXPECT_EQ(out.tag.quality, *want_quality) << "line " << line;
    EXPECT_EQ(out.tag.flags, *want_flags) << "line " << line;
}

std::optional<RawKind> raw_kind_named(const std::string& name) {
    static const std::map<std::string, RawKind> KINDS{
        {"signed", RawKind::SIGNED},
        {"unsigned", RawKind::UNSIGNED},
        {"float", RawKind::FLOAT},
        {"bool", RawKind::BOOL}};
    const auto found = KINDS.find(name);
    return found == KINDS.end() ? std::nullopt : std::optional(found->second);
}

void run_write(const PointRules& rules, const std::vector<std::string>& fields,
               std::size_t arrow, int line) {
    const std::vector<std::string> raw_type =
        split(field_value(fields, arrow, "raw_type"), ':');
    const auto value = value_of(field_value(fields, arrow, "value"));
    const auto kind =
        raw_type.size() == 2 ? raw_kind_named(raw_type[0]) : std::nullopt;
    const auto width = raw_type.size() == 2 ? integer<std::uint8_t>(raw_type[1])
                                            : std::nullopt;
    if (!value.has_value() || !kind.has_value() || !width.has_value()) {
        ADD_FAILURE() << "line " << line << " does not parse";
        return;
    }
    const WriteRules write{
        .tag_type = rules.tag_type,
        .raw_kind = *kind,
        .raw_width = *width,
        .invert = rules.invert,
        .scale = rules.scale.has_value() ? &*rules.scale : nullptr,
        .enum_codes = rules.enum_codes,
        .has_enum = rules.has_enum};
    const std::optional<WriteValue> result = to_raw_value(write, *value);
    const auto results = results_of(fields, arrow);
    if (results.contains("refused")) {
        EXPECT_FALSE(result.has_value()) << "line " << line;
        return;
    }
    const auto want_raw = integer<std::uint64_t>(result_field(results, "raw"));
    const auto want_written = value_of(result_field(results, "written"));
    if (!want_raw.has_value() || !want_written.has_value()) {
        ADD_FAILURE() << "line " << line << " does not parse";
        return;
    }
    if (!result.has_value()) {
        ADD_FAILURE() << "line " << line << ": refused";
        return;
    }
    EXPECT_EQ(result->raw.pattern, *want_raw) << "line " << line;
    EXPECT_TRUE(same(result->written, *want_written)) << "line " << line;
}

// The words of a line, without its comment.
std::vector<std::string> words_of(std::string text) {
    const std::size_t hash = text.find('#');
    if (hash != std::string::npos) {
        text.erase(hash);
    }
    std::istringstream words(text);
    std::vector<std::string> fields;
    for (std::string word; words >> word;) {
        fields.push_back(word);
    }
    return fields;
}

// Runs one `sample` or `write` line; false when it is not one.
bool run_vector(const std::map<std::string, NamedRules>& rules,
                const std::vector<std::string>& fields, int line) {
    const auto named = rules.find(fields[1]);
    const auto arrow = std::find(fields.begin(), fields.end(), "->");
    if (named == rules.end() || arrow == fields.end()) {
        return false;
    }
    const auto arrow_index = static_cast<std::size_t>(arrow - fields.begin());
    if (fields[0] == "sample") {
        run_sample(named->second.rules, fields, arrow_index, line);
        return true;
    }
    if (fields[0] == "write") {
        run_write(named->second.rules, fields, arrow_index, line);
        return true;
    }
    return false;
}

// Runs every line of the file; returns the number of vectors run.
int run_file(std::istream& file) {
    std::map<std::string, NamedRules> rules;
    std::string text;
    int line = 0;
    int vectors = 0;
    while (std::getline(file, text)) {
        ++line;
        const std::vector<std::string> fields = words_of(text);
        if (fields.empty()) {
            continue;
        }
        if (fields.size() < 2) {
            ADD_FAILURE() << "line " << line << " does not parse";
            continue;
        }
        if (fields[0] == "rules") {
            const std::optional<NamedRules> parsed = rules_of(fields);
            EXPECT_TRUE(parsed.has_value())
                << "line " << line << " does not parse";
            rules[fields[1]] = parsed.value_or(NamedRules{});
            continue;
        }
        EXPECT_TRUE(run_vector(rules, fields, line)) << "line " << line;
        ++vectors;
    }
    return vectors;
}

TEST(GoldenVectors, EveryVectorOfTheSharedFile) {
    std::ifstream file(SCADA_DRIVER_GOLDEN_VECTORS);
    ASSERT_TRUE(file.is_open()) << SCADA_DRIVER_GOLDEN_VECTORS;
    constexpr int MIN_VECTORS = 30;
    EXPECT_GT(run_file(file), MIN_VECTORS);
}

}  // namespace
}  // namespace scada::driver
