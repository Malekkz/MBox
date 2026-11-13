#ifndef SCADA_COMMON_RESULT_H
#define SCADA_COMMON_RESULT_H

#include "scada/common/panic.h"
#include "scada/common/status.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace scada {

// The outcome of an operation that returns a `T`: either the value or an
// error Status, never both and never neither.
//
// Check `ok()` before calling `value()`. Calling `value()` on an error is a
// programmer error and ends the process through `panic`; this check stays on
// in release builds. Use `Status` instead of `Result<void>`.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_void_v<T>, "use Status instead of Result<void>");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Status>,
                  "Result<Status> is ambiguous");

public:
    // Success, holding `value`. Implicit, so that a function can
    // `return value;`.
    // NOLINTNEXTLINE(google-explicit-constructor): see above
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    // Failure. `error` must not be OK; passing an OK status is a programmer
    // error and ends the process. Implicit, so that a function can
    // `return Status(...);`.
    // NOLINTNEXTLINE(google-explicit-constructor): see above
    Result(Status error) noexcept : status_(error) {
        if (status_.ok()) {
            panic("Result constructed from an OK status");
        }
    }

    // True if a value is held.
    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }

    // Returns the error, or an OK status if a value is held.
    [[nodiscard]] Status status() const noexcept { return status_; }

    // Return the held value. Precondition: `ok()`. The reference is valid
    // until the Result is destroyed, assigned to or moved from. The rvalue
    // overload lets `std::move(result).value()` move the value out.
    [[nodiscard]] T& value() & noexcept {
        require_value();
        return *value_;
    }
    [[nodiscard]] const T& value() const& noexcept {
        require_value();
        return *value_;
    }
    [[nodiscard]] T&& value() && noexcept {
        require_value();
        return std::move(*value_);
    }

private:
    void require_value() const noexcept {
        if (!value_.has_value()) {
            panic("Result::value() called on an error");
        }
    }

    std::optional<T> value_;
    Status status_;
};

}  // namespace scada

#endif  // SCADA_COMMON_RESULT_H
