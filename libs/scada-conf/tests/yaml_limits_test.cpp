// The default limits of the YAML subset parser at N and N + 1 (configuration
// schemas contract 1.3, ruling 10 on #100; B1 of the review of #130). Each
// makes a file of up to 8 MiB or a million nodes, so they are a test
// executable of their own, with its own time limit (#164).

#include "yaml_cases.h"

#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace scada::conf {
namespace {

using yaml_cases::code_at;
using yaml_cases::parse;

// B1: every line counts against the scalar limit before it is stored, and
// empty lines are counted, not stored.
TEST(Yaml, EmptyLinesOfABlockScalarCountAgainstTheLimit) {
    constexpr std::size_t LIMIT = 8;
    ParseLimits limits;
    limits.max_scalar_bytes = LIMIT;
    // "x\n" and six kept empty lines: 8 bytes.
    EXPECT_EQ(parse("a: |+\n x\n" + std::string(6, '\n'), limits),
              "{a: |\"x\\n\\n\\n\\n\\n\\n\\n\"}");
    EXPECT_EQ(parse("a: |+\n x\n" + std::string(7, '\n'), limits),
              "error 1:4 CF-113: a scalar exceeds its limit of 8 bytes");
    // Between two lines, the empty lines are in the value.
    EXPECT_EQ(parse("a: |\n x\n\n\n\n\n y\n", limits),
              "{a: |\"x\\n\\n\\n\\n\\ny\\n\"}");
    EXPECT_EQ(code_at("a: |\n x\n\n\n\n\n\n y\n", limits), "error 1:4 CF-113");
    // Stripped or clipped, trailing empty lines are not in the value and
    // cost nothing: 1,000,000 of them are accepted.
    const std::string many(1000000, '\n');
    EXPECT_EQ(parse("a: |\n x\n" + many, limits), "{a: |\"x\\n\"}");
    EXPECT_EQ(parse("a: >-\n x\n" + many, limits), "{a: >\"x\"}");
    EXPECT_EQ(parse("a: |\n" + many), "{a: |\"\"}");
}

// Ruling 10: the default limits at N and N + 1.
TEST(Yaml, TheDefaultEntryLimitAtNAndNPlusOne) {
    constexpr std::size_t ENTRIES = 65536;
    std::string list = "[0";
    for (std::size_t i = 1; i < ENTRIES; ++i) {
        list += ",0";
    }
    EXPECT_EQ(parse(list + "]").substr(0, 2), "[0");
    // Item 65,537 stands at column 2 x 65,537.
    EXPECT_EQ(code_at(list + ",0]"), "error 1:131074 CF-113");
}

TEST(Yaml, TheDefaultNodeLimitOfAFileAtNAndNPlusOne) {
    // A list of 20 lists, of 999,979 items in all: 1,000,000 nodes, each list
    // within the entry limit.
    constexpr std::size_t LISTS = 20;
    constexpr std::size_t ITEMS = 999979;
    constexpr std::size_t PER_LIST = 50000;
    std::string tree = "[\n";
    tree.reserve(3 * ITEMS);
    std::size_t left = ITEMS;
    for (std::size_t list = 0; list < LISTS; ++list) {
        const std::size_t count = std::min(left, PER_LIST);
        tree += "[0";
        for (std::size_t i = 1; i < count; ++i) {
            tree += ",0";
        }
        left -= count;
        tree += "],\n";
    }
    EXPECT_EQ(parse(tree + "]").substr(0, 3), "[[0");
    // One item more in the outer list.
    const std::string over = parse(tree + "0]");
    EXPECT_EQ(over.substr(over.find("CF-113")),
              "CF-113: the file exceeds its limit of 1000000 nodes");
}

TEST(Yaml, TheDefaultFileSizeAtNAndNPlusOne) {
    constexpr std::size_t MIB_8 = std::size_t{8} * 1024 * 1024;
    const std::string comment = "#" + std::string(MIB_8 - 1, 'c');
    EXPECT_EQ(parse(comment), "{}");
    EXPECT_EQ(parse(comment + "c"),
              "error 1:1 CF-113: the file exceeds its limit of 8 MiB");
}

}  // namespace
}  // namespace scada::conf
