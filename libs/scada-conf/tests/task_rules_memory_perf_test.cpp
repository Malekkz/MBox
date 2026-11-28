// The memory of the inputs the task rules read (D-125; M1 of the review of
// #189): 10,000 virtual tags `p.*`, and 10,000 virtual tags that each sum
// the pattern over them. The first form copied the matched names into every
// reader, 3.1 GB here; each pattern now keeps its matches once. The peak of
// the process is read, so this is an executable of its own. A performance
// test, with release flags. Invented examples only.

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <sys/resource.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace scada::conf {
namespace {

// The bound of the process's peak, in KiB: well under the bound of D-114
// (858 MB) and far below the 3.1 GB of the first form (D-125 gives the
// measured peak).
constexpr std::int64_t MAX_PEAK_KIB = std::int64_t{256} * 1024;

std::int64_t peak_kib() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return -1;
    }
    // KiB on Linux. glibc declares the field in a union of the C ABI.
    return usage.ru_maxrss;  // NOLINT(cppcoreguidelines-pro-type-union-access)
}

TEST(TaskRulesMemory, ManyReadersOfAPatternOfVirtualTags) {
    constexpr std::size_t TAGS = 10000;
    std::string tags = "tags:\n";
    for (std::size_t i = 0; i < TAGS; ++i) {
        tags += "  - { name: p.i" + std::to_string(i) +
                ", type: virtual, expr: \"1\" }\n";
    }
    for (std::size_t i = 0; i < TAGS; ++i) {
        tags += "  - { name: q.i" + std::to_string(i) +
                ", type: virtual, expr: \"sum(\\\"p.*\\\")\" }\n";
    }
    const Result<Validation> validation =
        validated({{"tags/site.yaml", tags},
                   {"system.yaml",
                    std::string(SYSTEM) + "limits: { max_tags: 100000 }\n"}});
    ASSERT_TRUE(validation.ok());
    EXPECT_EQ(validation.value().report.error_count, 0U);
    const std::int64_t peak = peak_kib();
    ASSERT_GT(peak, 0);
    RecordProperty("peak_kib", std::to_string(peak));
    EXPECT_LT(peak, MAX_PEAK_KIB);
}

}  // namespace
}  // namespace scada::conf
