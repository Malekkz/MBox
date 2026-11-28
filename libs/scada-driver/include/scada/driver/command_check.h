#ifndef SCADA_DRIVER_COMMAND_CHECK_H
#define SCADA_DRIVER_COMMAND_CHECK_H

// The checks of a write request on the command thread (driver interface
// contract 8.2, checks 2 to 9; decisions D-92, D-150). Review by a person:
// this is command handling.
//
// The first check that fails answers the request and stops it; nothing is
// sent to the device. Every check runs for every source. The library made
// check 1 (expiry, lifetime, a valid record) before the request was taken;
// check 10 (room in the worker's order queue) and check 11 (on the worker)
// come after these.
//
// Nothing here sends, posts or writes anything: `check_request` decides,
// from the request, its resolved target and what `CommandReads` reads at
// the check, and the command thread acts on the answer.

#include "scada/driver/compiled.h"
#include "scada/driver/failure.h"
#include "scada/driver/write_conversion.h"
#include "scada/tag/scada_tag.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scada::driver {

// The reason of a result (DI 8.8, D-85): `protocol_error` is the reason
// shifted 16 bits left, or'd with the device's raw code.
// NOLINTNEXTLINE(performance-enum-size): the contract's range is 0 to 32767
enum class Reason : std::uint16_t {
    NONE = 0,
    ALREADY_IN_POSITION = 1,
    NOTHING_TO_CANCEL = 2,
    UNKNOWN_TARGET = 10,
    WRONG_CONTROL_MODEL = 11,
    INVALID_VALUE = 12,
    NOT_SELECTED = 13,
    ALREADY_SELECTED = 14,
    SELECTION_MISMATCH = 15,
    SELECT_FAILED = 16,
    CONFIG_MISMATCH = 17,
    RATE_LIMITED = 18,
    BUSY = 19,
    REJECTED_INTERLOCK = 20,
    REJECTED_SYNCHROCHECK = 21,
    REJECTED_LOCAL = 22,
    REJECTED_MODE = 23,
    NOT_SUPPORTED = 24,
    COMM_FAIL = 25,
    SELECT_EXPIRED = 26,
    DEVICE_UNAVAILABLE = 27,
    SELECTED = 28,
    PARTIAL = 30,
    DEVICE_FAULT = 31,
    NO_RESPONSE = 40,
    TIMEOUT_CONFIRM = 41,
    TIMEOUT_TERMINATION = 42,
    EXPIRED_BEFORE_SEND = 50,
    NOT_STARTED = 60,
    LOCAL_AUTHORITY = 70,
};

// `protocol_error` of a result (D-85): `reason` in the upper half, the
// device's `raw` code in the lower; a raw code wider than 16 bits is
// 0xFFFF.
[[nodiscard]] std::int32_t protocol_error(Reason reason,
                                          std::uint32_t raw = 0) noexcept;

// An answer to a request: its status (SCADA_RESULT_*) and reason.
struct Answer {
    std::uint8_t status = SCADA_RESULT_OK;
    Reason reason = Reason::NONE;
    std::uint32_t raw = 0;

    friend bool operator==(const Answer&, const Answer&) = default;
};

// The modes of an output's mode tag (DI 8.2, check 5; coding agreed with CS).
enum class OutputMode : std::uint8_t { AUTO = 1, MANUAL = 2, REMOTE = 3 };

// A read of a tag at a check: the library's return code and the value.
struct TagReading {
    int code = SCADA_E_NOT_FOUND;
    scada_value value{};
};

// The reads the checks make, at the check (DI 8.2): the output's mode tag
// (`<output>.mode`, CS 12.5; owned by `scada-cmd`, by the tests in phase 2)
// and the panel input of its area. The command thread reads the tag
// database; a unit test gives what it wants.
class CommandReads {
public:
    CommandReads() = default;
    CommandReads(const CommandReads&) = delete;
    CommandReads& operator=(const CommandReads&) = delete;
    CommandReads(CommandReads&&) = delete;
    CommandReads& operator=(CommandReads&&) = delete;
    virtual ~CommandReads() = default;

    [[nodiscard]] virtual TagReading mode(
        const CompiledOutput& output) noexcept = 0;
    [[nodiscard]] virtual TagReading panel(
        const CompiledOutput& output) noexcept = 0;
};

// The write limits of one point (DI 8.9; CS `write_limits`): at least
// `min_interval` between counted writes, at most `max_per_day` in any 24 h.
// Counted in memory only: a restart starts a new count (8.9). The 24 h are
// a sliding window of the last `max_per_day` counted writes (D-150).
class WriteLimiter {
public:
    // Allocates the window once. May throw std::bad_alloc.
    explicit WriteLimiter(WriteLimits limits);

    // True when a write at `now_ns` (monotonic) is within the limits.
    [[nodiscard]] bool allows(std::int64_t now_ns) const noexcept;

    // Counts a write whose first byte was sent at `now_ns` (8.9).
    void count(std::int64_t now_ns) noexcept;

private:
    WriteLimits limits_;
    std::vector<std::int64_t> times_;  // the last `max_per_day` writes, a ring
    std::size_t next_ = 0;             // where the next one goes
    std::size_t counted_ = 0;          // how many of `times_` are used
    bool any_ = false;                 // a write was counted
    std::int64_t last_ns_ = 0;         // the last counted one
};

// The target of a request, resolved by its tag handle (check 2). Null
// pointers: the handle names nothing of this driver that the operation
// can act on.
struct CheckTarget {
    const CompiledDevice* device = nullptr;
    const CompiledPoint* point = nullptr;
    const CompiledOutput* output = nullptr;       // null for a READ_NOW
    DeviceStatus state = DeviceStatus::DISABLED;  // the device's, now
    const WriteLimiter* limiter = nullptr;        // the point's
};

// What the checks decided: an answer the command thread posts at once, or
// an order for the worker (`order` true, `answer` unused), with the raw
// value of a setpoint.
struct CheckOutcome {
    bool order = false;
    Answer answer{};
    WriteValue value{};
};

// Runs checks 2 to 9 of DI 8.2, in that order, on `request` and its
// `target` at `now_ns` (monotonic). Reads the mode tag for check 5, and the
// panel input for check 6 when the request is AUTO and the output's area
// has `local_blocks_auto`. Until slice 7 no selection is ever live: a SELECT
// is answered NOT_SUPPORTED, an OPERATE NOT_SELECTED, a CANCEL OK with
// NOTHING_TO_CANCEL (8.6, rules 2 and 6; D-150).
[[nodiscard]] CheckOutcome check_request(const scada_write_request& request,
                                         const CheckTarget& target,
                                         CommandReads& reads,
                                         std::int64_t now_ns) noexcept;

}  // namespace scada::driver

#endif  // SCADA_DRIVER_COMMAND_CHECK_H
