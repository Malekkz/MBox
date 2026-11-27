// The tags under a prefix (D-116, D-117): the stored tags and the tags of
// devices and channels, which are not stored, in the order of names, for
// the patterns of slice 7b. Invented examples only.

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/validate.h"
#include "tag_names.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {
namespace {

constexpr std::string_view METER =
    "template: meter_a\nprotocol: modbus\npoints:\n"
    "  - { name: flow, address: { table: holding, register: 1, type: uint16 "
    "} }\n";

// The names under `prefix` of a tree of two devices, `ps1.m1` and
// `ps1.m1x`, on line1, and `ps1.m2` on line2.
std::vector<std::string> names_under(std::string_view prefix) {
    const std::string line2 =
        "channel:\n  name: line2\n  type: rtu\n"
        "  device: /dev/serial/by-path/platform-serial2\n  baud: 9600\n"
        "devices:\n  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n";
    const Result<Validation> validation = validated(
        {{"templates/meter_a.yaml", std::string(METER)},
         {"drivers/modbus/line1.yaml",
          channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                  "  - { name: ps1.m1x, template: meter_a, unit_id: 3 }\n")},
         {"drivers/modbus/line2.yaml", line2}});
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    const std::optional<ExpandedTree>& expanded = validation.value().expanded;
    if (!expanded) {
        return {"not expanded"};
    }
    Diagnostics diagnostics;
    const TagNames tags = collect_tags(*expanded, {}, diagnostics);
    std::vector<std::string> names;
    tags.each_with_prefix(prefix,
                          [&names](std::string_view name, const TagInfo& info) {
                              EXPECT_FALSE(info.value_type.empty()) << name;
                              names.emplace_back(name);
                          });
    return names;
}

// N5 of the delta check of #163: a device in `sys` (CF-301 at its name)
// stores the tags of its points, and one of them may have the name of
// another device's tag. The name is visited once, as stored: a pattern
// that matches only it is compared once.
TEST(TagNamesPrefix, AStoredNameOfADeviceTagIsVisitedOnce) {
    const std::string meter =
        "template: meter_s\nprotocol: modbus\npoints:\n"
        "  - { name: state, address: { table: holding, register: 1, type: "
        "uint16 } }\n";
    const Result<Validation> validation = validated(
        {{"templates/meter_s.yaml", meter},
         {"drivers/modbus/line1.yaml",
          channel("  - { name: ps1.m1, template: meter_s, unit_id: 1 }\n"
                  "  - { name: sys.dev.ps1.m1, template: meter_s, unit_id: 2 "
                  "}\n")},
         {"tags/site.yaml",
          "tags:\n  - { name: v.n, type: virtual, expr: "
          "\"count_good(\\\"sys.dev.ps1.m1.st*\\\")\" }\n"}});
    ASSERT_TRUE(validation.ok());
    const Report& report = validation.value().report;
    ASSERT_EQ(report.errors.size(), 1U);
    EXPECT_EQ(format(report.errors.front()),
              "drivers/modbus/line1.yaml:8:13: error CF-301: tag names "
              "starting with sys. are reserved");
    EXPECT_EQ(validation.value().run_time.comparisons, 1U);
}

// A prefix within one device's tags gives its tags only, in name order.
TEST(TagNamesPrefix, ADevicesTags) {
    EXPECT_EQ(names_under("sys.dev.ps1.m1.re"),
              (std::vector<std::string>{
                  "sys.dev.ps1.m1.refusals", "sys.dev.ps1.m1.requests",
                  "sys.dev.ps1.m1.requests_per_scan",
                  "sys.dev.ps1.m1.response_ms", "sys.dev.ps1.m1.restarts"}));
    // `refresh_failures` needs a refreshed output, the identity tags
    // `identify`: neither is there.
    EXPECT_EQ(names_under("sys.dev.ps1.m1.ref"),
              std::vector<std::string>{"sys.dev.ps1.m1.refusals"});
}

// A prefix that two device names start with gives both devices' tags; a
// device whose name is a dotted beginning of the prefix is found too.
TEST(TagNamesPrefix, DevicesWhoseNamesStartAlike) {
    const std::vector<std::string> state = names_under("sys.dev.ps1.m1");
    EXPECT_EQ(state.size(), 2U * 17U);
    EXPECT_TRUE(std::ranges::is_sorted(state));
    EXPECT_EQ(names_under("sys.dev.ps1.m1x.st"),
              std::vector<std::string>{"sys.dev.ps1.m1x.state"});
}

// The point tags, stored, and a channel's period tags, made from its
// groups, merge in name order.
TEST(TagNamesPrefix, StoredAndMadeTagsInOrder) {
    EXPECT_EQ(names_under("ps1."),
              (std::vector<std::string>{"ps1.m1.flow", "ps1.m1x.flow",
                                        "ps1.m2.flow"}));
    EXPECT_EQ(names_under("sys.drv.line2.period."),
              (std::vector<std::string>{"sys.drv.line2.period.fast",
                                        "sys.drv.line2.period.normal",
                                        "sys.drv.line2.period.slow"}));
    const std::vector<std::string> all = names_under("sys.");
    EXPECT_TRUE(std::ranges::is_sorted(all));
    // 21 process tags of one protocol, 2 channels of 8, 3 devices of 17.
    EXPECT_EQ(all.size(), 21U + (2U * 8U) + (3U * 17U));
    EXPECT_TRUE(names_under("sys.dev.ps1.m9").empty());
}

// S2 of the review of #163: `set_value_type` gives a type to a tag of a
// tag file only; a point's tag keeps its own.
TEST(TagNamesPrefix, OnlyATagFilesTagTakesAType) {
    const Result<Validation> validation =
        validated({{"templates/meter_a.yaml", std::string(METER)},
                   {"drivers/modbus/line1.yaml",
                    channel("  - { name: ps1.m1, template: meter_a, "
                            "unit_id: 1 }\n")}});
    ASSERT_TRUE(validation.ok());
    for (const Diagnostic& error : validation.value().report.errors) {
        ADD_FAILURE() << format(error);
    }
    const std::optional<ExpandedTree>& expanded = validation.value().expanded;
    ASSERT_TRUE(expanded.has_value());
    Diagnostics diagnostics;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above
    TagNames tags = collect_tags(*expanded, {}, diagnostics);
    const std::optional<TagInfo> before = tags.find("ps1.m1.flow");
    ASSERT_TRUE(before.has_value());
    tags.set_value_type("ps1.m1.flow", "bool");
    const std::optional<TagInfo> after = tags.find("ps1.m1.flow");
    ASSERT_TRUE(after.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access): asserted above
    EXPECT_EQ(after->value_type, before->value_type);
    EXPECT_NE(after->value_type, "bool");
    // NOLINTEND(bugprone-unchecked-optional-access)
}

}  // namespace
}  // namespace scada::conf
