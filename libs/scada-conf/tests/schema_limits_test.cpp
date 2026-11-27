// The cost of the schema check of typed keys (B2 of the review of #130): it
// validates files of 4,096 and 65,536 keys three times each, so it is a
// test executable of its own, with its own time limit (#164).

#include "schema_cases.h"

#include "scada/conf/diagnostic.h"
#include "scada/conf/node.h"
#include "scada/conf/schema.h"
#include "scada/conf/types.h"
#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <ctime>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace scada::conf {
namespace {

using schema_cases::one_schema;

// The processor time of this thread, which other load does not change.
std::chrono::nanoseconds thread_time() {
    timespec now{};
    static_cast<void>(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now));
    return std::chrono::seconds(now.tv_sec) +
           std::chrono::nanoseconds(now.tv_nsec);
}

// B2: typed keys are checked with a sorted set. From 4,096 to 65,536 keys a
// linearithmic check takes about 17.5 times as long and a quadratic one 256
// times; the bound sits between, in processor time of this thread.
TEST(Schema, TypedKeysAreCheckedInLinearithmicTime) {
    constexpr std::size_t SMALL = 4096;
    constexpr std::size_t LARGE = 65536;
    constexpr int RUNS = 3;
    constexpr double MAX_RATIO = 64.0;
    const SchemaSet set = one_schema(R"({ "type": "object",
        "x-scada-key-type": "integer",
        "unevaluatedProperties": { "type": "string" } })");
    const auto fastest = [&set](std::size_t keys) {
        std::string yaml;
        for (std::size_t i = 0; i < keys; ++i) {
            yaml += std::to_string(i) + ": x\n";
        }
        Diagnostics diagnostics;
        const std::optional<Document> document =
            parse_yaml(yaml, "f.yaml", diagnostics);
        if (!document.has_value()) {
            ADD_FAILURE() << "no document";
            return std::chrono::nanoseconds::max();
        }
        auto best = std::chrono::nanoseconds::max();
        for (int run = 0; run < RUNS; ++run) {
            ScalarTypes types(*document);
            const auto start = thread_time();
            set.validate(*document, types, "t.schema.json", "f.yaml",
                         diagnostics);
            best = std::min(best, thread_time() - start);
        }
        EXPECT_EQ(diagnostics.error_count(), 0U);
        return best;
    };
    const auto small = fastest(SMALL);
    const auto large = fastest(LARGE);
    EXPECT_LT(static_cast<double>(large.count()),
              MAX_RATIO * static_cast<double>(small.count()))
        << "small " << small.count() << " ns, large " << large.count() << " ns";
}

}  // namespace
}  // namespace scada::conf
