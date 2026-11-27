#include "scada/driver/identity.h"

#include "scada/driver/tag_value.h"
#include "scada/driver/text_pattern.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace scada::driver {
namespace {

using tag::TagType;

// -1, 0 or 1.
template <class Number>
int order(Number left, Number right) noexcept {
    if (left < right) {
        return -1;
    }
    return right < left ? 1 : 0;
}

// The two values of one rule's point, compared as `compare_value` does.
int compare_read(const TagValue& left, const TagValue& right,
                 bool& comparable) noexcept {
    comparable = left.type == right.type;
    if (!comparable) {
        return 0;
    }
    switch (left.type) {
        case TagType::BOOL:
            return order(left.b, right.b);
        case TagType::INT64:
            return order(left.i, right.i);
        case TagType::UINT64:
            return order(left.u, right.u);
        case TagType::FLOAT64:
            comparable = !std::isnan(left.f) && !std::isnan(right.f);
            return comparable ? order(left.f, right.f) : 0;
        case TagType::STRING:
        default:
            comparable = false;
            return 0;
    }
}

TagValue as_read(const ExpectedValue& value) noexcept {
    return TagValue{.type = value.type,
                    .b = value.b,
                    .i = value.i,
                    .u = value.u,
                    .f = value.f,
                    .text = value.text};
}

}  // namespace

bool same_value(const ExpectedValue& expected, const TagValue& read) noexcept {
    if (expected.type != read.type) {
        return false;
    }
    if (read.type == TagType::STRING) {
        return std::string_view(expected.text) == read.text;
    }
    bool comparable = false;
    const int sign = compare_read(read, as_read(expected), comparable);
    return comparable && sign == 0;
}

int compare_value(const TagValue& read, const ExpectedValue& bound,
                  bool& comparable) noexcept {
    return compare_read(read, as_read(bound), comparable);
}

bool identity_matches(const IdentityCheck& check,
                      const TagValue& read) noexcept {
    switch (check.kind) {
        case IdentityCheck::Kind::EQUALS:
            return !check.values.empty() &&
                   same_value(check.values.front(), read);
        case IdentityCheck::Kind::IN:
            return std::any_of(check.values.begin(), check.values.end(),
                               [&](const ExpectedValue& value) {
                                   return same_value(value, read);
                               });
        case IdentityCheck::Kind::RANGE: {
            if (check.values.size() != 2) {
                return false;
            }
            bool low_ok = false;
            bool high_ok = false;
            const int low = compare_value(read, check.values[0], low_ok);
            const int high = compare_value(read, check.values[1], high_ok);
            return low_ok && high_ok && low >= 0 && high <= 0;
        }
        case IdentityCheck::Kind::PATTERN:
            return read.type == TagType::STRING &&
                   text_matches(check.pattern, read.text);
        default:
            return false;
    }
}

RestartWatch::RestartWatch(std::span<const RestartRule> rules)
    : rules_(rules), last_(rules.size()) {}

void RestartWatch::reset() noexcept {
    for (Last& last : last_) {
        last.seen = false;
        // A flag found set after a connect is that connect's, covered by
        // its identity check: it is no rising edge.
        last.flag_set = true;
    }
}

bool RestartWatch::sample(std::uint32_t point, const TagValue& value) noexcept {
    bool restarted = false;
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const RestartRule& rule = rules_[index];
        if (rule.point != point) {
            continue;
        }
        Last& last = last_[index];
        TagValue before = last.value;
        before.text = std::string_view(last.text.data(), last.text_length);
        switch (rule.kind) {
            case RestartRule::Kind::DECREASES: {
                bool comparable = false;
                const int sign = compare_read(value, before, comparable);
                restarted = restarted || (last.seen && comparable && sign < 0);
                break;
            }
            case RestartRule::Kind::CHANGES: {
                bool comparable = false;
                const bool differs =
                    value.type == TagType::STRING
                        ? value.type != before.type || value.text != before.text
                        : compare_read(value, before, comparable) != 0 ||
                              !comparable;
                restarted = restarted || (last.seen && differs);
                break;
            }
            case RestartRule::Kind::EQUALS: {
                const bool set = same_value(rule.value, value);
                // On the first sample after a reset the flag is taken as it
                // stands: only a later rise is a restart.
                restarted = restarted || (last.seen && set && !last.flag_set);
                last.flag_set = set;
                break;
            }
            default:
                break;
        }
        last.seen = true;
        last.value = value;
        last.value.text = {};
        last.text_length = std::min(value.text.size(), last.text.size());
        std::copy_n(value.text.data(), last.text_length, last.text.data());
    }
    return restarted;
}

}  // namespace scada::driver
