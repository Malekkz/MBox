// Unit tests for Result<T>.

#include "scada/common/result.h"
#include "scada/common/status.h"

#include <gtest/gtest.h>

#include <csignal>
#include <memory>
#include <string>
#include <utility>

namespace {

using scada::Result;
using scada::Status;
using scada::StatusCode;

constexpr int SEVEN = 7;
constexpr int ANSWER = 42;

Result<int> parse_positive(int value) {
    if (value <= 0) {
        return Status(StatusCode::OUT_OF_RANGE, "value is not positive");
    }
    return value;
}

TEST(Result, HoldsAValue) {
    const Result<int> result = parse_positive(SEVEN);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), SEVEN);
    EXPECT_TRUE(result.status().ok());
}

TEST(Result, HoldsAnError) {
    const Result<int> result = parse_positive(-1);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), StatusCode::OUT_OF_RANGE);
    EXPECT_EQ(result.status().message(), "value is not positive");
}

TEST(Result, ValueCanBeModified) {
    Result<std::string> result = std::string("abc");
    result.value() += "def";
    EXPECT_EQ(result.value(), "abcdef");
}

TEST(Result, MoveOnlyValueGoesInAndOut) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(ANSWER);
    ASSERT_TRUE(result.ok());
    const std::unique_ptr<int> taken = std::move(result).value();
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(*taken, ANSWER);
}

TEST(Result, CanBeMoved) {
    Result<std::string> source = std::string("moved");
    const Result<std::string> target = std::move(source);
    ASSERT_TRUE(target.ok());
    EXPECT_EQ(target.value(), "moved");
}

// Misuse ends the process instead of returning garbage.

TEST(ResultDeathTest, ValueOnAnErrorAborts) {
    const Result<int> result = parse_positive(0);
    // NOLINTNEXTLINE(scada-banned): the goto is inside the framework's death-test macro
    EXPECT_EXIT(static_cast<void>(result.value()),
                testing::KilledBySignal(SIGABRT),
                "Result::value\\(\\) called on an error");
}

TEST(ResultDeathTest, ConstructionFromAnOkStatusAborts) {
    // NOLINTNEXTLINE(scada-banned): the goto is inside the framework's death-test macro
    EXPECT_EXIT(static_cast<void>(Result<int>(Status())),
                testing::KilledBySignal(SIGABRT),
                "Result constructed from an OK status");
}

}  // namespace
