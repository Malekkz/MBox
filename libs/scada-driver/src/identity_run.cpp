#include "scada/driver/identity_run.h"

#include "scada/driver/identity.h"
#include "scada/driver/identity_text.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace scada::driver {

IdentityRun::IdentityRun(std::span<const IdentityCheck> checks)
    : checks_(checks), reads_(checks.size()) {}

void IdentityRun::start(std::int64_t deadline_ns) noexcept {
    for (Read& read : reads_) {
        read.state = CheckRead::NOT_READ;
        read.has_value = false;
        read.length = 0;
    }
    deadline_ns_ = deadline_ns;
}

void IdentityRun::sample(std::uint32_t point, Outcome outcome, bool valid,
                         const TagValue& value) noexcept {
    for (std::size_t index = 0; index < checks_.size(); ++index) {
        if (checks_[index].point != point) {
            continue;
        }
        Read& read = reads_[index];
        switch (outcome) {
            case Outcome::COMM_FAIL:
                continue;  // no verdict: read again (ruling item 3)
            case Outcome::ABSENT:
                read.state = CheckRead::ABSENT;
                read.has_value = false;
                continue;
            case Outcome::NO_VALUE:
            case Outcome::DEVICE_INVALID:
                read.state = CheckRead::UNREADABLE;
                read.has_value = false;
                continue;
            case Outcome::VALUE:
                break;
        }
        if (!valid) {
            read.state = CheckRead::UNREADABLE;
            read.has_value = false;
            continue;
        }
        // The value as the identity text shows it: a STRING cleaned first,
        // so that a pattern and an equality both see the cleaned text
        // (ruling item 4).
        read.value = value;
        read.value.text = {};
        read.length = value.type == tag::TagType::STRING
                          ? clean_identity_text(value.text, read.text)
                          : 0;
        read.has_value = true;
        read.state = identity_matches(checks_[index], view(read))
                         ? CheckRead::MATCHED
                         : CheckRead::WRONG;
    }
}

TagValue IdentityRun::view(const Read& read) noexcept {
    TagValue out = read.value;
    if (out.type == tag::TagType::STRING) {
        out.text = std::string_view(read.text.data(), read.length);
    }
    return out;
}

IdentityVerdict IdentityRun::verdict(std::int64_t now_ns) const noexcept {
    bool all_matched = true;
    bool unreadable = false;
    std::uint32_t first_unreadable = 0;
    for (std::size_t index = 0; index < reads_.size(); ++index) {
        const auto check = static_cast<std::uint32_t>(index);
        switch (reads_[index].state) {
            case CheckRead::WRONG:
                return {.kind = IdentityVerdict::Kind::MISMATCH,
                        .reason = MismatchReason::VALUE,
                        .check = check};
            case CheckRead::ABSENT:
                return {.kind = IdentityVerdict::Kind::MISMATCH,
                        .reason = MismatchReason::ABSENT,
                        .check = check};
            case CheckRead::UNREADABLE:
                if (!unreadable) {
                    first_unreadable = check;
                }
                unreadable = true;
                all_matched = false;
                break;
            case CheckRead::NOT_READ:
                all_matched = false;
                break;
            case CheckRead::MATCHED:
                break;
        }
    }
    if (all_matched) {
        return {.kind = IdentityVerdict::Kind::MATCH};
    }
    if (unreadable && now_ns >= deadline_ns_) {
        return {.kind = IdentityVerdict::Kind::MISMATCH,
                .reason = MismatchReason::UNREADABLE,
                .check = first_unreadable};
    }
    return {};
}

namespace {

// The largest n for which n fields and n - 1 separators leave each field at
// least one byte of the text.
constexpr std::size_t shown_fields() noexcept {
    std::size_t fields = 1;
    while ((fields + 1) + fields * IDENTITY_SEPARATOR.size() <=
           TAG_STRING_MAX_BYTES) {
        ++fields;
    }
    return fields;
}

}  // namespace

std::size_t IdentityRun::text(
    std::span<char, TAG_STRING_MAX_BYTES> out) const noexcept {
    // One field per check, at most as many as can each get a byte of the
    // text after the separators: 84 for 250 bytes, 4.7 KB of stack in place
    // of 7 KB (review of PR 171). A check past them shows nothing; it would
    // get no byte of the text anyway.
    constexpr std::size_t MAX_FIELDS = shown_fields();
    std::array<IdentityField, MAX_FIELDS> fields{};
    const std::size_t count = std::min(reads_.size(), fields.size());
    const std::span<IdentityField> used = std::span(fields).first(count);
    for (std::size_t index = 0; index < count; ++index) {
        used[index] = IdentityField{.read = reads_[index].has_value,
                                    .value = view(reads_[index])};
    }
    return join_identity(used, out);
}

std::size_t IdentityRun::check_text(std::uint32_t check,
                                    std::span<char> out) const noexcept {
    if (check >= reads_.size() || !reads_[check].has_value) {
        return 0;
    }
    return identity_field(view(reads_[check]), out);
}

}  // namespace scada::driver
