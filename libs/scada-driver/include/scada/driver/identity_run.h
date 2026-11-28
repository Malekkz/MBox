#ifndef SCADA_DRIVER_IDENTITY_RUN_H
#define SCADA_DRIVER_IDENTITY_RUN_H

// One device's identity check as it runs (driver interface contract 3.6;
// decision D-147, ruling items 2 to 4): what each check's point read in the
// current round, the verdict that follows, and the text of
// `sys.dev.<device>.identity`. The engine gives it the samples of the
// identity cycle and asks for the verdict once the cycle is done.

#include "scada/common/realtime.h"
#include "scada/driver/identity.h"
#include "scada/driver/identity_text.h"
#include "scada/driver/raw_value.h"
#include "scada/driver/tag_value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scada::driver {

// What a check's point gave in the current round.
enum class CheckRead : std::uint8_t {
    NOT_READ = 0,  // nothing yet, or no usable answer (a communication failure)
    MATCHED,       // a valid value that passes the check
    WRONG,         // a valid value that does not pass it
    ABSENT,        // the address is not on the device
    UNREADABLE,    // NO_VALUE, DEVICE_INVALID, or a value processing refused
};

// Why a check failed (the log record's REASON, ruling item 3).
enum class MismatchReason : std::uint8_t {
    VALUE = 1,   // value
    ABSENT,      // absent
    UNREADABLE,  // unreadable
};

struct IdentityVerdict {
    enum class Kind : std::uint8_t {
        NONE = 0,  // not yet: read the cycle again
        MATCH,
        MISMATCH,
    };
    Kind kind = Kind::NONE;
    MismatchReason reason = MismatchReason::VALUE;  // for MISMATCH
    std::uint32_t check = 0;  // the first failing check, for MISMATCH
};

class IdentityRun {
public:
    // `checks` is borrowed and outlives the run. Allocates its buffers.
    explicit IdentityRun(std::span<const IdentityCheck> checks);

    // A new round of the check, to end by `deadline_ns` (ruling item 2):
    // every check NOT_READ. Never allocates.
    SCADA_RT void start(std::int64_t deadline_ns) noexcept;

    // The sample of point `point`: its outcome, and for VALUE the value
    // processing gave it (`valid`: processing took it). A sample of a point
    // no check reads is ignored. A COMM_FAIL leaves the check NOT_READ.
    SCADA_RT void sample(std::uint32_t point, Outcome outcome, bool valid,
                         const TagValue& value) noexcept;

    // The verdict of a round whose cycle is done, at `now_ns`:
    // - a WRONG or ABSENT check: MISMATCH at once (value, absent);
    // - every check MATCHED: MATCH;
    // - an UNREADABLE check: NONE until the deadline, then MISMATCH
    //   (unreadable);
    // - otherwise (a check not read): NONE.
    // The first failing check, in configuration order, is named.
    [[nodiscard]] SCADA_RT IdentityVerdict
    verdict(std::int64_t now_ns) const noexcept;

    // The text of `sys.dev.<device>.identity`: the values read in the round,
    // cleaned and joined (identity_text.h); a check not read is an empty
    // field. Returns the length.
    [[nodiscard]] SCADA_RT std::size_t text(
        std::span<char, TAG_STRING_MAX_BYTES> out) const noexcept;

    // Check `check`'s value as the identity text shows it, for the log
    // record, into `out`: cleaned, a number in decimal; nothing when it
    // read no value. Returns the length.
    [[nodiscard]] SCADA_RT std::size_t check_text(
        std::uint32_t check, std::span<char> out) const noexcept;

    [[nodiscard]] std::int64_t deadline_ns() const noexcept {
        return deadline_ns_;
    }
    [[nodiscard]] std::size_t checks() const noexcept { return checks_.size(); }

private:
    // A read value. A STRING's text is kept in `text`, cleaned, and `value`
    // holds no view of it: a run may be moved, so the view is made when it
    // is used (`view`).
    struct Read {
        CheckRead state = CheckRead::NOT_READ;
        bool has_value = false;
        TagValue value{};
        std::array<char, TAG_STRING_MAX_BYTES> text{};
        std::size_t length = 0;
    };

    // `read`'s value, a STRING's text a view of its buffer.
    [[nodiscard]] static TagValue view(const Read& read) noexcept;

    std::span<const IdentityCheck> checks_;
    std::vector<Read> reads_;
    std::int64_t deadline_ns_ = 0;
};

}  // namespace scada::driver

#endif  // SCADA_DRIVER_IDENTITY_RUN_H
