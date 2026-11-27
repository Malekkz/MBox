// The work of stage 5 on run-time expressions at sizes the default limits
// allow (D-117; M1 to M3 and S2 of the review of #163): tens of thousands
// of tags and patterns, so an executable of its own with its own time
// limit. Each case bounds the work counted (`Validation::run_time`), not
// the time it takes. Invented examples only.

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace scada::conf {
namespace {

constexpr std::string_view AREAS = "areas:\n  ps1: { description: Station }\n";

// The tags of a tag file: `count` of `make(i)`, split into files of at most
// 20,000 tags, below the size limit of a file.
void add_tags(Files& files, std::string_view stem, std::size_t count,
              const auto& make) {
    constexpr std::size_t PER_FILE = 20000;
    for (std::size_t first = 0; first < count; first += PER_FILE) {
        std::string text = "tags:\n";
        for (std::size_t i = first; i < count && i < first + PER_FILE; ++i) {
            text += make(i);
        }
        files["tags/" + std::string(stem) + std::to_string(first) + ".yaml"] =
            text;
    }
}

std::string internal(std::string_view name) {
    return "  - { name: " + std::string(name) +
           ", type: internal, value_type: int64 }\n";
}

std::string virtual_tag(std::string_view name, std::string_view expr) {
    return "  - { name: " + std::string(name) + ", type: virtual, expr: \"" +
           std::string(expr) + "\" }\n";
}

// The work of a tree, with no error expected.
RunTimeCounts work_of(Files files) {
    files.try_emplace("areas.yaml", std::string(AREAS));
    const Result<Validation> validation = validated(files);
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {};
    }
    EXPECT_EQ(validation.value().report.error_count, 0U)
        << (validation.value().report.errors.empty()
                ? std::string()
                : validation.value().report.errors.front().message);
    return validation.value().run_time;
}

// M1: 10,000 virtual tags that each sum one pattern over 10,000 virtual
// tags. The pattern is one vertex of the graph, walked once, and resolved
// once: about 20,000 comparisons, where one edge per pair was 100,000,000.
TEST(RunTimeLimits, APatternIsOneVertexOfTheGraph) {
    constexpr std::size_t TAGS = 10000;
    Files files;
    add_tags(files, "p", TAGS, [](std::size_t index) {
        return virtual_tag("p.i" + std::to_string(index) + ".v", "1");
    });
    add_tags(files, "q", TAGS, [](std::size_t index) {
        return virtual_tag("q.i" + std::to_string(index), R"(sum(\"p.*.v\"))");
    });
    const RunTimeCounts work = work_of(files);
    EXPECT_LE(work.comparisons, 2 * TAGS);
    EXPECT_EQ(work.kept_patterns, 1U);
}

// M2 and M3: a pattern is resolved once whatever its uses, and kept as a
// summary of fixed size: 10 uses and 2,000 do the same work.
TEST(RunTimeLimits, APatternIsResolvedOnceForAllItsUses) {
    constexpr std::size_t TAGS = 5000;
    const auto with_uses = [](std::size_t uses) {
        Files files;
        add_tags(files, "r", TAGS, [](std::size_t index) {
            return internal("r.i" + std::to_string(index));
        });
        add_tags(files, "u", uses, [](std::size_t index) {
            return virtual_tag("u.j" + std::to_string(index),
                               R"(sum(\"r.*\"))");
        });
        return work_of(files);
    };
    const RunTimeCounts few = with_uses(10);
    const RunTimeCounts many = with_uses(2000);
    EXPECT_EQ(few.comparisons, TAGS);
    EXPECT_EQ(many.comparisons, TAGS);
    EXPECT_EQ(many.kept_patterns, 1U);
}

// M2: distinct patterns are each resolved once against the names that
// start with their prefix: the work is their number times those names (on
// #136, which no limit of 1.3 bounds), and each is kept as one summary.
TEST(RunTimeLimits, DistinctPatternsAreEachResolvedOnce) {
    constexpr std::size_t TAGS = 2000;
    constexpr std::size_t PATTERNS = 500;
    Files files;
    add_tags(files, "r", TAGS, [](std::size_t index) {
        return internal("r.i" + std::to_string(index));
    });
    add_tags(files, "u", 2 * PATTERNS, [](std::size_t index) {
        return virtual_tag(
            "u.j" + std::to_string(index),
            "count_good(\\\"r.i*" + std::to_string(index % PATTERNS) + "\\\")");
    });
    const RunTimeCounts work = work_of(files);
    EXPECT_EQ(work.comparisons, PATTERNS * TAGS);
    EXPECT_EQ(work.kept_patterns, PATTERNS);
}

}  // namespace
}  // namespace scada::conf
