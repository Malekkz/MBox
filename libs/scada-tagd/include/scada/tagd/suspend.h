#ifndef SCADA_TAGD_SUSPEND_H
#define SCADA_TAGD_SUSPEND_H

// Detection of a system suspend (docs/contracts/tag-database.md, section
// 3.4; decision D-44). The unit must not suspend or hibernate, because
// CLOCK_MONOTONIC, on which every age and timeout of the tag database is
// measured, does not advance meanwhile. Masking the sleep targets cannot be
// checked from inside a unit, so scada-tagd looks for the traces: the
// kernel's list of sleep states, and CLOCK_BOOTTIME, which counts the time
// spent suspended, running ahead of CLOCK_MONOTONIC. A paused virtual
// machine stops both clocks and is not seen.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scada::tagd {

// Where the kernel lists the sleep states it offers.
inline constexpr std::string_view SLEEP_STATES_PATH = "/sys/power/state";

// One reading of the two clocks: CLOCK_MONOTONIC, then CLOCK_BOOTTIME, then
// CLOCK_MONOTONIC again, in ns.
struct ClockSample {
    std::int64_t monotonic_before_ns = 0;
    std::int64_t boottime_ns = 0;
    std::int64_t monotonic_after_ns = 0;
};

// Reads the two clocks as ClockSample describes.
[[nodiscard]] ClockSample sample_clocks() noexcept;

// What a sample showed.
struct SuspendFinding {
    // True for the first sample: the time was spent suspended before
    // scada-tagd started, since boot.
    bool at_start = false;
    // Time spent suspended that the sample proves: since boot at the first
    // sample, since the previous sample otherwise. A lower bound, in ns.
    std::int64_t suspended_ns = 0;

    friend bool operator==(const SuspendFinding&,
                           const SuspendFinding&) = default;
};

// Follows the difference CLOCK_BOOTTIME minus CLOCK_MONOTONIC, which is the
// time the system spent suspended since boot. The difference lies between
// `boottime - monotonic_after` and `boottime - monotonic_before`; only what
// lies beyond the reading interval is reported, so the time between the
// reads never counts as a suspend.
class SuspendWatch {
public:
    // Takes one sample. Returns a finding when the first sample shows time
    // suspended since boot, or when a later one shows that this time grew
    // since the previous sample; nothing otherwise. A sample whose clocks
    // go back is ignored.
    [[nodiscard]] std::optional<SuspendFinding> observe(
        const ClockSample& sample) noexcept;

private:
    bool started_ = false;
    // The upper bound of the difference at the previous sample.
    std::int64_t upper_ns_ = 0;
};

// The sleep states the kernel offers, read from `path` (normally
// /sys/power/state), with white space at the ends removed. Empty if the
// file is empty, missing or cannot be read: such a kernel cannot suspend,
// or cannot say. The contents are outside input: at most 256 bytes are
// read, and bytes outside printable ASCII are replaced by '?'.
[[nodiscard]] std::string read_sleep_states(const std::string& path);

}  // namespace scada::tagd

#endif  // SCADA_TAGD_SUSPEND_H
