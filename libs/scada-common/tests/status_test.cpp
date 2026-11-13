#include "scada/common/status.h"
#include "check.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace {

using scada::Status;
using scada::StatusCode;

// A Status must be usable in real-time code and in constant expressions.
static_assert(std::is_trivially_copyable_v<Status>);
static_assert(std::is_nothrow_default_constructible_v<Status>);
static_assert(Status().ok());
static_assert(!Status(StatusCode::TIMEOUT, "late").ok());
static_assert(Status(StatusCode::TIMEOUT, "late").message() == "late");

Status fails_with_not_found() {
    return {StatusCode::NOT_FOUND, "tag not found"};
}

}  // namespace

int main() {
    scada::test::Checker checker;

    {
        const Status status;
        checker.expect(status.ok(), "default: ok");
        checker.expect(status.code() == StatusCode::OK, "default: code");
        checker.expect(status.message().empty(), "default: empty message");
    }
    {
        const Status status = fails_with_not_found();
        checker.expect(!status.ok(), "error: not ok");
        checker.expect(status.code() == StatusCode::NOT_FOUND, "error: code");
        checker.expect(status.message() == "tag not found", "error: message");

        const Status copy = status;
        checker.expect(copy.code() == StatusCode::NOT_FOUND &&
                           copy.message() == "tag not found",
                       "copy keeps code and message");
    }
    {
        struct Name {
            StatusCode code;
            std::string_view text;
        };
        const std::array<Name, 14> names{{
            {StatusCode::OK, "OK"},
            {StatusCode::INVALID_ARGUMENT, "INVALID_ARGUMENT"},
            {StatusCode::OUT_OF_RANGE, "OUT_OF_RANGE"},
            {StatusCode::NOT_FOUND, "NOT_FOUND"},
            {StatusCode::ALREADY_EXISTS, "ALREADY_EXISTS"},
            {StatusCode::PERMISSION_DENIED, "PERMISSION_DENIED"},
            {StatusCode::FAILED_PRECONDITION, "FAILED_PRECONDITION"},
            {StatusCode::RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED"},
            {StatusCode::UNAVAILABLE, "UNAVAILABLE"},
            {StatusCode::TIMEOUT, "TIMEOUT"},
            {StatusCode::IO_ERROR, "IO_ERROR"},
            {StatusCode::DATA_CORRUPT, "DATA_CORRUPT"},
            {StatusCode::UNSUPPORTED, "UNSUPPORTED"},
            {StatusCode::INTERNAL, "INTERNAL"},
        }};
        for (const Name& name : names) {
            checker.expect(scada::to_string(name.code) == name.text, name.text);
        }
        // A value that is not an enumerator, as an unvalidated read from
        // outside would give.
        constexpr std::uint8_t NOT_AN_ENUMERATOR = 200;
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the out-of-range value is what is tested; the fixed underlying type makes the cast defined
        const auto unknown = static_cast<StatusCode>(NOT_AN_ENUMERATOR);
        checker.expect(scada::to_string(unknown) == "UNKNOWN",
                       "to_string: value outside the enumeration");
    }

    return checker.exit_code();
}
