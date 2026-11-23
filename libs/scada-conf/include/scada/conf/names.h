#ifndef SCADA_CONF_NAMES_H
#define SCADA_CONF_NAMES_H

// The name grammars of configuration schemas contract 2.1, and the other
// text forms the schemas check (addresses, zones, times of day).

#include <cstddef>
#include <string_view>

namespace scada::conf {

// Lengths of section 2.1, in bytes.
constexpr std::size_t MAX_TAG_NAME = 127;
constexpr std::size_t MAX_DEVICE_NAME = 100;
constexpr std::size_t MAX_IDENTIFIER = 31;
constexpr std::size_t MAX_POLL_GROUP_NAME = 8;
constexpr std::size_t MAX_PROTOCOL_NAME = 10;
constexpr std::size_t MAX_UNIT_ID = 31;
constexpr std::size_t MAX_TEXT = 1024;
constexpr std::size_t MAX_HOST_NAME = 253;

// `[a-z][a-z0-9_]*`, 1 to 31 bytes.
[[nodiscard]] bool is_identifier(std::string_view text) noexcept;

// Segments `[a-z0-9][a-z0-9_-]*` joined by `.`, 1 to `max_length` bytes:
// 127 for tag and point names, 100 for device names.
[[nodiscard]] bool is_tag_name(std::string_view text,
                               std::size_t max_length = MAX_TAG_NAME) noexcept;

// `[a-z0-9][a-z0-9_]*`, not all digits, 1 to 8 bytes: poll group and tier
// names.
[[nodiscard]] bool is_poll_group_name(std::string_view text) noexcept;

// `[a-z0-9]+`, 1 to 10 bytes.
[[nodiscard]] bool is_protocol_name(std::string_view text) noexcept;

// `[a-z0-9][a-z0-9-]*`, 1 to 31 bytes.
[[nodiscard]] bool is_unit_id(std::string_view text) noexcept;

// A process name of the tag set (section 12.2): `tagd`, `control`, `cmd`,
// `alarm`, `hist`, `api`, `audit`, `io`, or `drv-<protocol>`.
[[nodiscard]] bool is_process_name(std::string_view text) noexcept;

// Free text (2.1): UTF-8 without control characters (C0, DEL, C1). The
// parser has checked the encoding; escapes may still give control
// characters.
[[nodiscard]] bool is_free_text(std::string_view text) noexcept;

// An IPv4 address in dotted decimal (four parts 0 to 255, no leading
// zeros).
[[nodiscard]] bool is_ipv4_address(std::string_view text) noexcept;

// An IPv6 address in the text forms of RFC 4291 2.2 (with `::` and an IPv4
// tail), without a zone index.
[[nodiscard]] bool is_ipv6_address(std::string_view text) noexcept;

// An IPv4 or IPv6 address.
[[nodiscard]] bool is_ip_address(std::string_view text) noexcept;

// A host name of RFC 1123: labels of letters, digits and `-`, not starting
// or ending with `-`, 1 to 63 bytes each, at most 253 bytes in all; not all
// digits and dots (so that a bad IPv4 address is not a host name).
[[nodiscard]] bool is_host_name(std::string_view text) noexcept;

// An address with a prefix length: `<IPv4>/<0..32>` or `<IPv6>/<0..128>`.
[[nodiscard]] bool is_cidr(std::string_view text) noexcept;

// The form of an IANA time zone name: components of letters, digits, `_`,
// `-` and `+` separated by `/`, each starting with a letter, 1 to 14 bytes
// (the tz database's own rule). Whether the zone exists on the unit is not
// checked (D-107).
[[nodiscard]] bool is_time_zone_name(std::string_view text) noexcept;

// A time of day `HH:MM`, 00:00 to 23:59.
[[nodiscard]] bool is_time_of_day(std::string_view text) noexcept;

// An `https://` URL with a host and no white space.
[[nodiscard]] bool is_https_url(std::string_view text) noexcept;

}  // namespace scada::conf

#endif  // SCADA_CONF_NAMES_H
