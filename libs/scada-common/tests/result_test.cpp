#include "check.h"
#include "scada/common/result.h"

#include <memory>
#include <string>
#include <utility>

namespace {

using scada::Result;
using scada::Status;
using scada::StatusCode;

Result<int> parse_positive(int value) {
    if (value <= 0) {
        return Status(StatusCode::OUT_OF_RANGE, "value is not positive");
    }
    return value;
}

}  // namespace

int main() {
    scada::test::Checker checker;

    {
        const Result<int> result = parse_positive(7);
        checker.expect(result.ok(), "value: ok");
        checker.expect(result.value() == 7, "value: content");
        checker.expect(result.status().ok(), "value: status is OK");
    }
    {
        const Result<int> result = parse_positive(-1);
        checker.expect(!result.ok(), "error: not ok");
        checker.expect(result.status().code() == StatusCode::OUT_OF_RANGE,
                       "error: code");
        checker.expect(result.status().message() == "value is not positive",
                       "error: message");
    }
    {
        Result<std::string> result = std::string("abc");
        result.value() += "def";
        checker.expect(result.value() == "abcdef", "value can be modified");
    }
    {
        // A move-only value goes in and comes out without a copy.
        Result<std::unique_ptr<int>> result = std::make_unique<int>(42);
        checker.expect(result.ok(), "move-only: ok");
        const std::unique_ptr<int> taken = std::move(result).value();
        checker.expect(taken != nullptr && *taken == 42,
                       "move-only: moved out");
    }
    {
        Result<std::string> source = std::string("moved");
        const Result<std::string> target = std::move(source);
        checker.expect(target.ok() && target.value() == "moved",
                       "a Result can be moved");
    }

    // Misuse ends the process instead of returning garbage.
    checker.expect(scada::test::aborts([] {
                       const Result<int> result = parse_positive(0);
                       static_cast<void>(result.value());
                   }),
                   "value() on an error aborts");
    checker.expect(scada::test::aborts([] {
                       const Result<int> result = Status();
                       static_cast<void>(result.ok());
                   }),
                   "construction from an OK status aborts");
    checker.expect(!scada::test::aborts([] {
                       const Result<int> result = parse_positive(1);
                       static_cast<void>(result.value());
                   }),
                   "correct use does not abort");

    return checker.exit_code();
}
