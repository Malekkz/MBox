// Unit tests for Status and StatusCode.

#include "scada/common/status.h"

#include <gtest/gtest.h>

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

TEST(Status, DefaultIsOk) {
    const Status status;
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::OK);
    EXPECT_TRUE(status.message().empty());
}

TEST(Status, ErrorCarriesCodeAndMessage) {
    const Status status = fails_with_not_found();
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::NOT_FOUND);
    EXPECT_EQ(status.message(), "tag not found");
}

TEST(Status, CopyKeepsCodeAndMessage) {
    const Status status = fails_with_not_found();
    const Status copy = status;
    EXPECT_EQ(copy.code(), StatusCode::NOT_FOUND);
    EXPECT_EQ(copy.message(), "tag not found");
}

TEST(Status, EveryCodeHasItsName) {
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
        EXPECT_EQ(scada::to_string(name.code), name.text);
    }
}

TEST(Status, ValueOutsideTheEnumerationIsUnknown) {
    // As an unvalidated read from outside would give.
    constexpr std::uint8_t NOT_AN_ENUMERATOR = 200;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the out-of-range value is what is tested; the fixed underlying type makes the cast defined
    const auto unknown = static_cast<StatusCode>(NOT_AN_ENUMERATOR);
    EXPECT_EQ(scada::to_string(unknown), "UNKNOWN");
}

}  // namespace
