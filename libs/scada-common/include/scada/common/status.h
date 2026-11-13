#ifndef SCADA_COMMON_STATUS_H
#define SCADA_COMMON_STATUS_H

#include "scada/common/static_string.h"

#include <cstdint>
#include <string_view>

namespace scada {

// Why an operation failed. The set is deliberately small: a caller branches
// on the code, and a person reads the message.
enum class StatusCode : std::uint8_t {
    OK = 0,
    INVALID_ARGUMENT,     // the caller passed a value that is never valid
    OUT_OF_RANGE,         // a value is outside the allowed limits
    NOT_FOUND,            // the named thing does not exist
    ALREADY_EXISTS,       // the thing to create is already there
    PERMISSION_DENIED,    // the caller is not allowed to do this
    FAILED_PRECONDITION,  // the system is not in a state that allows this
    RESOURCE_EXHAUSTED,   // out of memory, queue space, descriptors or quota
    UNAVAILABLE,          // cannot be done now; trying again later may work
    TIMEOUT,              // not finished within the allowed time
    IO_ERROR,             // the operating system or a device reported an error
    DATA_CORRUPT,         // stored or received data failed validation
    UNSUPPORTED,          // valid, but not implemented or not enabled
    INTERNAL,             // a defect in this software
};

// Returns the name of `code`, for example "TIMEOUT", or "UNKNOWN" for a value
// that is not an enumerator. The view refers to static storage.
[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

// The outcome of an operation that returns no value: success, or an error
// code with a fixed message.
//
// A Status owns nothing, never allocates and is trivially copyable, so it can
// be created and returned by real-time code. The message is a literal that
// names what failed; details that vary at run time (names, numbers) belong in
// the log record written where the error is handled.
class [[nodiscard]] Status {
public:
    // Success.
    constexpr Status() noexcept = default;

    // An outcome with the given code. `message` should say what failed, in
    // lower case and without a trailing full stop.
    constexpr Status(StatusCode code, StaticString message) noexcept
        : message_(message), code_(code) {}

    // True if the operation succeeded.
    [[nodiscard]] constexpr bool ok() const noexcept {
        return code_ == StatusCode::OK;
    }

    [[nodiscard]] constexpr StatusCode code() const noexcept { return code_; }

    // Returns the message; empty for a default-constructed Status. The view
    // refers to static storage.
    [[nodiscard]] constexpr std::string_view message() const noexcept {
        return message_.view();
    }

private:
    StaticString message_;
    StatusCode code_ = StatusCode::OK;
};

}  // namespace scada

#endif  // SCADA_COMMON_STATUS_H
