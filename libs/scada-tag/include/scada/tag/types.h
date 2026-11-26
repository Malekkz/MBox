#ifndef SCADA_TAG_TYPES_H
#define SCADA_TAG_TYPES_H

// Enumerations of the tag database that a user of the tag interface sees
// (docs/contracts/tag-database.md, sections 6 and 9.4).
//
// The shared memory holds these as plain integers, and what is read from it
// is input (coding standard, section 9). The `*_from_raw` functions are the
// only way from such an integer to an enumeration: they return nothing for a
// value that is not an enumerator.

#include "scada/common/realtime.h"

#include <cstdint>
#include <optional>
#include <type_traits>

namespace scada::tag {

// Type of a tag value (section 6).
enum class TagType : std::uint8_t {
    BOOL = 1,
    INT64 = 2,
    UINT64 = 3,
    FLOAT64 = 4,
    STRING = 5,
};

// Quality of a tag value (section 6).
enum class Quality : std::uint8_t {
    GOOD = 0,
    UNCERTAIN = 1,
    BAD = 2,
};

// Bits of `quality_flags` (section 6). A value holds any combination.
enum class QualityFlag : std::uint16_t {
    STALE = 1U << 0U,
    COMM_FAIL = 1U << 1U,
    OUT_OF_RANGE = 1U << 2U,
    // Not from the source: entered by a person, substituted by
    // configuration, or reported by the device as forced (contract 1.1).
    SUBSTITUTED = 1U << 3U,
    NOT_INIT = 1U << 4U,
    OWNER_DOWN = 1U << 5U,
    TIME_UNCERTAIN = 1U << 6U,
    BLOCKED = 1U << 7U,     // out of service or blocked for transmission
    TEST = 1U << 8U,        // produced in test mode
    OVERFLOW = 1U << 9U,    // counter rolled over or value exceeded its format
    RESTORED = 1U << 10U,   // retained value, not yet written by the owner
    LOCAL_TIME = 1U << 11U  // source time set by the owner, not a device
};

// Every bit that is a QualityFlag.
inline constexpr std::uint16_t QUALITY_FLAGS_MASK = 0x0FFFU;

// `WriteRequest.operation` (section 9.4).
enum class WriteOperation : std::uint8_t {
    DIRECT = 1,
    SELECT = 2,
    OPERATE = 3,
    CANCEL = 4,
    READ_NOW = 5,
};

// `WriteRequest.source` (section 9.4).
enum class WriteSource : std::uint8_t {
    AUTO = 1,
    OPERATOR = 2,
    REMOTE = 3,
};

// `WriteResult.status` (section 9.4).
enum class WriteStatus : std::uint8_t {
    OK = 1,
    REJECTED = 2,
    TIMEOUT = 3,
    DEVICE_ERROR = 4,
    NOT_AUTHORISED = 5,
    EXPIRED = 6,
    ABORTED = 7,
};

// Returns the integer that the shared memory holds for `value`.
template <typename Enum>
    requires std::is_enum_v<Enum>
SCADA_RT [[nodiscard]] constexpr std::underlying_type_t<Enum> to_raw(
    Enum value) noexcept {
    return static_cast<std::underlying_type_t<Enum>>(value);
}

// Returns the enumerator of `Enum` with the value `raw`, or nothing if `raw`
// is outside `first` to `last`. For enumerations whose values have no gaps;
// use the named functions below, which fix the range.
template <typename Enum>
    requires std::is_enum_v<Enum>
SCADA_RT [[nodiscard]] constexpr std::optional<Enum> enum_from_raw(
    std::underlying_type_t<Enum> raw, Enum first, Enum last) noexcept {
    if (raw < to_raw(first) || raw > to_raw(last)) {
        return std::nullopt;
    }
    return static_cast<Enum>(raw);
}

// Each returns the enumerator with the value `raw`, or nothing if there is
// none.
SCADA_RT [[nodiscard]] constexpr std::optional<TagType> tag_type_from_raw(
    std::uint8_t raw) noexcept {
    return enum_from_raw(raw, TagType::BOOL, TagType::STRING);
}

SCADA_RT [[nodiscard]] constexpr std::optional<Quality> quality_from_raw(
    std::uint8_t raw) noexcept {
    return enum_from_raw(raw, Quality::GOOD, Quality::BAD);
}

SCADA_RT [[nodiscard]] constexpr std::optional<WriteOperation>
write_operation_from_raw(std::uint8_t raw) noexcept {
    return enum_from_raw(raw, WriteOperation::DIRECT, WriteOperation::READ_NOW);
}

SCADA_RT [[nodiscard]] constexpr std::optional<WriteSource>
write_source_from_raw(std::uint8_t raw) noexcept {
    return enum_from_raw(raw, WriteSource::AUTO, WriteSource::REMOTE);
}

SCADA_RT [[nodiscard]] constexpr std::optional<WriteStatus>
write_status_from_raw(std::uint8_t raw) noexcept {
    return enum_from_raw(raw, WriteStatus::OK, WriteStatus::ABORTED);
}

// True if `flags` has no bit that is not a QualityFlag.
SCADA_RT [[nodiscard]] constexpr bool quality_flags_valid(
    std::uint16_t flags) noexcept {
    return (flags & ~QUALITY_FLAGS_MASK) == 0;
}

// True if `flag` is set in `flags`.
SCADA_RT [[nodiscard]] constexpr bool has_flag(std::uint16_t flags,
                                               QualityFlag flag) noexcept {
    return (flags & to_raw(flag)) != 0;
}

}  // namespace scada::tag

#endif  // SCADA_TAG_TYPES_H
