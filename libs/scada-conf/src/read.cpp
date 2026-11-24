#include "scada/conf/read.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/conf/node.h"
#include "scada/conf/scalar.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace scada::conf {

namespace {

constexpr unsigned MAX_WIDTH = 64;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1000000;

Status not_of_form() {
    return {StatusCode::INVALID_ARGUMENT, "the scalar has no form of the type"};
}

Status out_of_range() {
    return {StatusCode::OUT_OF_RANGE, "the value does not fit the type"};
}

// The text of a plain scalar, or nothing.
std::optional<std::string_view> plain_text(const Document& document,
                                           NodeId node) noexcept {
    if (document.kind(node) != NodeKind::SCALAR ||
        document.style(node) != ScalarStyle::PLAIN) {
        return std::nullopt;
    }
    return document.text(node);
}

// A plain integer: INVALID_ARGUMENT if the scalar is not of the integer
// form, OUT_OF_RANGE if it is but does not fit 64 bits (D-107).
Result<Integer> plain_integer(const Document& document, NodeId node) {
    const std::optional<std::string_view> text = plain_text(document, node);
    const IntegerRead read = text ? read_integer(*text) : IntegerRead{};
    if (read.form == IntegerForm::NOT_INTEGER) {
        return not_of_form();
    }
    if (read.form == IntegerForm::TOO_LARGE) {
        return out_of_range();
    }
    return read.value;
}

}  // namespace

Result<std::int64_t> to_int64(const Document& document, NodeId node) {
    const Result<Integer> value = plain_integer(document, node);
    if (!value.ok()) {
        return value.status();
    }
    const std::optional<std::int64_t> fits = as_int64(value.value());
    if (!fits) {
        return out_of_range();
    }
    return *fits;
}

Result<std::uint64_t> to_uint64(const Document& document, NodeId node) {
    const Result<Integer> value = plain_integer(document, node);
    if (!value.ok()) {
        return value.status();
    }
    const std::optional<std::uint64_t> fits = as_uint64(value.value());
    if (!fits) {
        return out_of_range();
    }
    return *fits;
}

Result<double> to_decimal(const Document& document, NodeId node) {
    const std::optional<std::string_view> text = plain_text(document, node);
    if (!text) {
        return not_of_form();
    }
    const IntegerRead integer = read_integer(*text);
    if (integer.form == IntegerForm::TOO_LARGE) {
        return out_of_range();
    }
    if (integer.form == IntegerForm::FITS) {
        const auto magnitude = static_cast<double>(integer.value.magnitude);
        return integer.value.negative ? -magnitude : magnitude;
    }
    const std::optional<double> decimal = parse_decimal(*text);
    if (!decimal) {
        return not_of_form();
    }
    return *decimal;
}

Result<bool> to_bool(const Document& document, NodeId node) {
    const std::optional<std::string_view> text = plain_text(document, node);
    const std::optional<bool> value =
        text ? parse_boolean(*text) : std::nullopt;
    if (!value) {
        return not_of_form();
    }
    return *value;
}

Result<std::int64_t> to_duration_ns(const Document& document, NodeId node) {
    if (document.kind(node) != NodeKind::SCALAR) {
        return not_of_form();
    }
    const std::optional<std::uint64_t> milliseconds =
        parse_duration_ms(document.text(node));
    if (!milliseconds) {
        return not_of_form();
    }
    constexpr auto LIMIT = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max() / NANOSECONDS_PER_MILLISECOND);
    if (*milliseconds > LIMIT) {
        return out_of_range();
    }
    return static_cast<std::int64_t>(*milliseconds) *
           NANOSECONDS_PER_MILLISECOND;
}

Result<std::uint64_t> to_raw_pattern(const Document& document, NodeId node,
                                     unsigned width_bits) {
    if (width_bits == 0 || width_bits > MAX_WIDTH) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "a raw pattern is 1 to 64 bits wide");
    }
    const Result<Integer> read = plain_integer(document, node);
    if (!read.ok()) {
        return read.status();
    }
    const Integer* value = &read.value();
    const std::uint64_t mask = width_bits == MAX_WIDTH
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : (std::uint64_t{1} << width_bits) - 1;
    if (!value->negative) {
        if (value->magnitude > mask) {
            return out_of_range();
        }
        return value->magnitude;
    }
    // -2^(width - 1) is the most negative value of the width.
    const std::uint64_t most_negative = std::uint64_t{1} << (width_bits - 1);
    if (value->magnitude > most_negative) {
        return out_of_range();
    }
    return (~value->magnitude + 1) & mask;
}

bool is_integer_form(const Document& document, NodeId node) {
    const std::optional<std::string_view> text = plain_text(document, node);
    if (!text) {
        return false;
    }
    return read_integer(*text).form != IntegerForm::NOT_INTEGER;
}

}  // namespace scada::conf
