#ifndef SCADA_CONF_SRC_NAME_PATTERN_H
#define SCADA_CONF_SRC_NAME_PATTERN_H

// Name patterns (configuration schemas contract 2.5): `*` matches any
// characters within one segment, `?` exactly one character of a segment.
// Shared by the points a health rule applies to (stage 4) and the patterns
// of aggregates in run-time expressions (stage 5, D-117).

#include <cstdint>
#include <string_view>

namespace scada::conf {

// True if `name` matches `pattern`, the whole of both. Linear in their
// lengths times the number of `*` retried, with no allocation.
[[nodiscard]] bool matches_name_pattern(std::string_view pattern,
                                        std::string_view name);

// As above, and adds to `steps` the steps the match took: one per character
// of `name` compared, retried after a `*` or passed, for a budget of the
// work (D-117).
[[nodiscard]] bool matches_name_pattern(std::string_view pattern,
                                        std::string_view name,
                                        std::uint64_t& steps);

// True if `text` holds `*` or `?`, so that it is a pattern and not a name.
[[nodiscard]] bool is_name_pattern(std::string_view text) noexcept;

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_NAME_PATTERN_H
