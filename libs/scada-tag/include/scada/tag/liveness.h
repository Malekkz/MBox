#ifndef SCADA_TAG_LIVENESS_H
#define SCADA_TAG_LIVENESS_H

// Liveness of a process of the tag database
// (docs/contracts/tag-database.md, section 10.1).

#include <cstdint>
#include <string_view>

namespace scada::tag {

// Whether a process is down, and by which rule of section 10.1. The rules
// are tested in this order, and the first that holds is reported.
enum class DownReason : std::uint8_t {
    NONE = 0,         // not down: region usable, RUNNING, heartbeat fresh
    REGION_MISSING,   // its `proc` region is missing, unusable or not ready
    NOT_RUNNING,      // its state is not RUNNING
    HEARTBEAT_STALE,  // its heartbeat is older than its timeout
};

// Returns a short text for `reason`, for example "heartbeat stale", or
// "unknown" for a value that is not an enumerator. The view refers to static
// storage.
[[nodiscard]] std::string_view to_string(DownReason reason) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_LIVENESS_H
