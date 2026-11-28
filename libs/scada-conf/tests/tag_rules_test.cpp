// Stage 5: retained values, logging and alarms of the tags (configuration
// schemas contract 5.2, 6 and 8; decision D-119; slice 7c of #100).

#include "tag_rules.h"
#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"
#include "scada/conf/tree.h"
#include "scada/conf/types.h"
#include "scada/conf/validate.h"
#include "scada/conf/yaml.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {
namespace {

constexpr std::string_view AREAS = "areas:\n  ps1: { description: Station }\n";

// meter_a with a number point, a BOOL point and a string point.
constexpr std::string_view METER =
    "template: meter_a\nprotocol: modbus\npoints:\n"
    "  - { name: flow, address: { table: holding, register: 1, type: uint16 "
    "} }\n"
    "  - { name: run, address: { table: coil, register: 2 } }\n"
    "  - { name: label, address: { table: holding, register: 3, type: "
    "string, length: 4 } }\n";

// The errors of the tree of `files`, with the areas, meter_a and one device
// of it.
Lines errors_of(Files files, std::string_view meter = METER) {
    files.try_emplace("areas.yaml", std::string(AREAS));
    files.try_emplace("templates/meter_a.yaml", std::string(meter));
    files.try_emplace("drivers/modbus/line1.yaml", one_device());
    Lines found;
    for (const std::string& line : expand(files)) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    return found;
}

// The warnings of the tree of `files`, which has no error. The area is
// given only to a tree that names it: an area nothing uses is CW-701.
Lines warnings_of(Files files, std::string_view meter = METER,
                  std::string_view devices = {}) {
    files.try_emplace("templates/meter_a.yaml", std::string(meter));
    files.try_emplace("drivers/modbus/line1.yaml",
                      devices.empty() ? one_device() : channel(devices));
    if (std::ranges::any_of(files, [](const auto& file) {
            return file.second.find("area: ps1") != std::string::npos;
        })) {
        files.try_emplace("areas.yaml", std::string(AREAS));
    }
    const Result<Validation> validation = validated(files);
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    EXPECT_EQ(validation.value().report.error_count, 0U);
    for (const Diagnostic& error : validation.value().report.errors) {
        ADD_FAILURE() << format(error);
    }
    Lines found;
    for (const Diagnostic& warning : validation.value().report.warnings) {
        found.push_back(format(warning));
    }
    return found;
}

Files tags(std::string_view text) {
    return {{"tags/site.yaml", "tags:\n" + std::string(text)}};
}

// --- Retained values (5.2) ---------------------------------------------

// A STRING tag cannot be retained (CF-507): at `retain`, or at the
// `value_type` of a manual tag, which is retained by default.
TEST(TagRules, AStringTagIsNotRetained) {
    EXPECT_EQ(errors_of(tags(
                  "  - { name: a, type: internal, value_type: string, retain: "
                  "true }\n"
                  "  - { name: b, type: manual, value_type: string, area: ps1 "
                  "}\n"
                  "  - { name: c, type: manual, value_type: string, area: "
                  "ps1, retain: false }\n"
                  "  - { name: d, type: manual, value_type: int64, area: ps1 "
                  "}\n")),
              (Lines{"tags/site.yaml:2:60: error CF-507: string tags cannot "
                     "be retained",
                     "tags/site.yaml:3:42: error CF-507: string tags cannot "
                     "be retained"}));
    // A point's string tag, at the instance.
    const std::string meter =
        std::string(METER) +
        "  - { name: text, retain: true, address: { table: holding, "
        "register: 5, type: string, length: 4 } }\n";
    EXPECT_EQ(errors_of({}, meter),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-507: string "
                    "tags cannot be retained\n  from template "
                    "templates/meter_a.yaml:7:27 (point text)"});
}

// `retain_now` and `retain_max_age` need `retain` (CF-506); `retain_now`
// is for internal and manual tags only (CF-509); `retain_max_age` is
// refused on a counter (CF-508).
TEST(TagRules, TheRetainKeys) {
    EXPECT_EQ(
        errors_of(tags(
            "  - { name: a, type: internal, value_type: int64, retain_now: "
            "true }\n"
            "  - { name: b, type: internal, value_type: int64, retain_now: "
            "true, retain: true }\n"
            "  - { name: c, type: internal, value_type: int64, retain_now: "
            "false }\n"
            "  - { name: d, type: virtual, expr: \"1\", retain: true, "
            "retain_now: true }\n"
            "  - { name: e, type: virtual, expr: \"1\", retain_now: false }\n"
            "  - { name: f, type: internal, value_type: int64, "
            "retain_max_age: 1h }\n"
            "  - { name: g, type: internal, value_type: uint64, kind: "
            "counter, retain: true, retain_max_age: 1h }\n"
            "  - { name: h, type: manual, value_type: int64, area: ps1, "
            "retain_max_age: 1h }\n"
            "  - { name: i, type: virtual, expr: \"1\", retain_now: true }\n")),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"tags/site.yaml:2:63: error CF-506: retain_now needs retain: "
               "true",
               "tags/site.yaml:5:68: error CF-509: retain_now is allowed on "
               "internal and manual tags only",
               "tags/site.yaml:6:54: error CF-509: retain_now is allowed on "
               "internal and manual tags only",
               "tags/site.yaml:7:67: error CF-506: retain_max_age needs "
               "retain: true",
               "tags/site.yaml:8:97: error CF-508: retain_max_age is not "
               "allowed on counter tags",
               "tags/site.yaml:10:54: error CF-509: retain_now is allowed on "
               "internal and manual tags only"}));
    // A counter point of a template, at its instance.
    const std::string meter =
        std::string(METER) +
        "  - { name: energy, kind: counter, retain_max_age: 1d, address: { "
        "table: holding, register: 5, type: uint32 } }\n";
    EXPECT_EQ(
        errors_of({}, meter),
        (Lines{"drivers/modbus/line1.yaml:7:11: error CF-506: retain_max_age "
               "needs retain: true\n  from template "
               "templates/meter_a.yaml:7:52 (point energy)",
               "drivers/modbus/line1.yaml:7:11: error CF-508: retain_max_age "
               "is not allowed on counter tags\n  from template "
               "templates/meter_a.yaml:7:52 (point energy)"}));
}

// Every `retain_max_age` is CW-510 until #89; a template's is one warning
// for all its instances.
TEST(TagRules, AnAgeBoundHasNoEffectYet) {
    EXPECT_EQ(warnings_of(tags("  - { name: a, type: manual, value_type: "
                               "int64, area: ps1, retain_max_age: 1h }\n")),
              Lines{"tags/site.yaml:2:76: warning CW-510: retain_max_age has "
                    "no effect until the tag database supports it (#89)"});
    const std::string meter =
        std::string(METER) +
        "  - { name: total, retain: true, retain_max_age: 1d, address: { "
        "table: holding, register: 5, type: uint32 } }\n";
    EXPECT_EQ(
        warnings_of({}, meter,
                    "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                    "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n"),
        Lines{"drivers/modbus/line1.yaml:7:13: warning CW-510: retain_max_age "
              "has no effect until the tag database supports it (#89)\n  "
              "from template templates/meter_a.yaml:7:50 (point total)"});
}

// Past `limits.max_tags` the tags are not whole: the rules that read a
// tag's type are not checked, the others are.
TEST(TagRules, PastTheTagLimit) {
    Files files = tags(
        "  - { name: a, type: internal, value_type: string, retain: true }\n"
        "  - { name: b, type: internal, value_type: int64, retain_now: true "
        "}\n");
    files.emplace("system.yaml",
                  std::string(SYSTEM) + "limits: { max_tags: 3 }\n");
    files.emplace("alarms/site.yaml",
                  "alarms:\n  - { tag: ps1.m1.run, type: high, limit: 1, "
                  "priority: high, area: ps1 }\n");
    const Lines found = errors_of(files);
    ASSERT_EQ(found.size(), 2U);
    EXPECT_NE(found.front().find("CF-1001"), std::string::npos)
        << found.front();
    EXPECT_EQ(found.back(),
              "tags/site.yaml:3:63: error CF-506: retain_now needs retain: "
              "true");
}

// --- Logging (6) -------------------------------------------------------

// Each tier's interval is longer than the one before and a whole multiple
// of it (CF-801).
TEST(TagRules, AggregateTiers) {
    const auto with = [](std::string_view tiers) {
        return errors_of(
            {{"logging.yaml", "aggregate_tiers:\n" + std::string(tiers)}});
    };
    EXPECT_EQ(with("  - { name: 1m, interval: 1m }\n"
                   "  - { name: 15m, interval: 15m }\n"
                   "  - { name: 1h, interval: 1h }\n"),
              Lines{});
    EXPECT_EQ(with("  - { name: a, interval: 1m }\n"
                   "  - { name: b, interval: 90s }\n"
                   "  - { name: c, interval: 90s }\n"
                   "  - { name: d, interval: 30s }\n"),
              (Lines{"logging.yaml:3:26: error CF-801: aggregate tier \"b\": "
                     "its interval must be a whole multiple of that of \"a\"",
                     "logging.yaml:4:26: error CF-801: aggregate tier \"c\": "
                     "its interval must be longer than that of \"b\"",
                     "logging.yaml:5:26: error CF-801: aggregate tier \"d\": "
                     "its interval must be longer than that of \"c\""}));
}

constexpr std::string_view LOGGING =
    "policies:\n"
    "  changes: { mode: on_change, deadband: 0.5%, raw_retention: 90d }\n"
    "  sampled: { mode: periodic, interval: 1m, raw_retention: 1y }\n";

// Settings without `policy` are complete (CF-802); a STRING tag is logged
// on change only (CF-803), by its policy or by what it overrides; the
// percentage deadband of its policy gives no CF-505 (M1 of the review of
// #172).
TEST(TagRules, LoggingSettings) {
    Files files = tags(
        "  - { name: a, type: internal, value_type: int64, logging: { "
        "mode: on_change } }\n"
        "  - { name: b, type: internal, value_type: int64, logging: { "
        "interval: 1s } }\n"
        "  - { name: s1, type: internal, value_type: string, logging: "
        "sampled }\n"
        "  - { name: s2, type: internal, value_type: string, logging: { "
        "policy: sampled, mode: on_change } }\n"
        "  - { name: s3, type: internal, value_type: string, logging: { "
        "policy: changes, mode: both } }\n"
        "  - { name: s4, type: internal, value_type: string, logging: "
        "none }\n");
    files.emplace("logging.yaml", std::string(LOGGING));
    EXPECT_EQ(
        errors_of(files),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"tags/site.yaml:2:60: error CF-802: logging of \"a\" "
               "needs raw_retention",
               "tags/site.yaml:3:60: error CF-802: logging of \"b\" "
               "needs mode",
               "tags/site.yaml:3:60: error CF-802: logging of \"b\" "
               "needs raw_retention",
               "tags/site.yaml:4:62: error CF-803: string tag \"s1\" can "
               "be logged on change only",
               "tags/site.yaml:6:62: error CF-803: string tag \"s3\" can "
               "be logged on change only"}));
}

// The default policy is the policy of a tag that gives no `logging`, a
// point's too: a STRING tag under a periodic one is CF-803, at its name
// (D-119).
TEST(TagRules, TheDefaultPolicy) {
    Files files = tags(
        "  - { name: s, type: internal, value_type: string }\n"
        "  - { name: n, type: internal, value_type: int64 }\n");
    files.emplace("logging.yaml",
                  std::string(LOGGING) + "default_policy: sampled\n");
    EXPECT_EQ(errors_of(files),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-803: string "
                     "tag \"ps1.m1.label\" can be logged on change only\n  "
                     "from template templates/meter_a.yaml:6:13 (point label)",
                     "tags/site.yaml:2:13: error CF-803: string tag \"s\" can "
                     "be logged on change only"}));
}

// A percentage deadband written in the tag's own alarm or `logging` map
// needs the tag's `range` (CF-505), at the deadband; one from a policy does
// not (M1 of the review of #172), nor a deadband that is a number.
TEST(TagRules, APercentageNeedsRange) {
    Files files = tags(
        "  - { name: a, type: internal, value_type: float64, logging: changes "
        "}\n"
        "  - { name: b, type: internal, value_type: float64, range: [0, 10], "
        "logging: changes }\n"
        "  - { name: c, type: internal, value_type: float64, logging: { "
        "mode: on_change, deadband: 1%, raw_retention: 1d } }\n"
        "  - { name: d, type: internal, value_type: float64, logging: { "
        "policy: changes, deadband: 0.1 } }\n"
        "  - name: e\n    type: internal\n    value_type: float64\n"
        "    alarms: [ { type: high, limit: 4, deadband: 2%, priority: high, "
        "area: ps1 } ]\n"
        "  - name: f\n    type: internal\n    value_type: float64\n"
        "    range: [0, 100]\n"
        "    alarms: [ { type: high, limit: 4, deadband: 2%, priority: high, "
        "area: ps1 } ]\n"
        "  - name: g\n    type: internal\n    value_type: float64\n"
        "    alarms: [ { type: high, limit: 4, deadband: 0.5, priority: high, "
        "area: ps1 } ]\n");
    files.emplace("logging.yaml", std::string(LOGGING));
    files.emplace("alarms/site.yaml",
                  "alarms:\n  - { tag: ps1.m1.flow, type: high, limit: 4, "
                  "deadband: 1%, priority: high, area: ps1 }\n");
    EXPECT_EQ(
        errors_of(files),
        (Lines{"alarms/site.yaml:2:57: error CF-505: \"ps1.m1.flow\": a "
               "percentage needs range",
               "tags/site.yaml:4:91: error CF-505: \"c\": a percentage needs "
               "range",
               "tags/site.yaml:9:49: error CF-505: \"e\": a percentage needs "
               "range"}));
}

// An `interval` finer than the tag's refresh is CW-805, at the tag's own
// interval, else at its `logging`: a read point's poll group (`normal` by
// default), a virtual tag's task (`normal` by default), the task an
// internal tag names. A tag with no refresh these rules know gives none.
TEST(TagRules, AnIntervalFinerThanTheRefresh) {
    const std::string meter =
        std::string(METER) +
        "  - { name: p1, logging: { policy: sampled, interval: 500ms }, "
        "address: { table: holding, register: 5, type: uint16 } }\n"
        "  - { name: p2, poll_group: fast, logging: { policy: sampled, "
        "interval: 500ms }, address: { table: holding, register: 6, type: "
        "uint16 } }\n"
        "  - { name: p3, poll_group: slow, logging: fast, address: { table: "
        "holding, register: 7, type: uint16 } }\n";
    Files files = tags(
        "  - { name: v1, type: virtual, expr: \"1\", logging: fast }\n"
        "  - { name: v2, type: virtual, expr: \"1\", task: slow, logging: "
        "fast }\n"
        "  - { name: i1, type: internal, value_type: int64, task: slow, "
        "logging: { policy: sampled, interval: 200ms } }\n"
        "  - { name: i2, type: internal, value_type: int64, logging: fast }\n"
        "  - { name: m1, type: manual, value_type: int64, area: ps1, "
        "logging: fast }\n");
    files.emplace("logging.yaml",
                  std::string(LOGGING) +
                      "  fast: { mode: periodic, interval: 50ms, "
                      "raw_retention: 1d }\n");
    files.emplace("control/tasks.yaml",
                  "tasks:\n  normal: { period: 20ms, programs: [] }\n"
                  "  slow: { period: 1s, programs: [] }\n");
    EXPECT_EQ(
        warnings_of(files, meter),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"drivers/modbus/line1.yaml:7:11: warning CW-805: "
               "\"ps1.m1.p1\": interval 500ms is finer than the tag's "
               "refresh 1s\n  from template templates/meter_a.yaml:7:55 "
               "(point p1)",
               "drivers/modbus/line1.yaml:7:11: warning CW-805: "
               "\"ps1.m1.p3\": interval 50ms is finer than the tag's "
               "refresh 10s\n  from template templates/meter_a.yaml:9:44 "
               "(point p3)",
               "tags/site.yaml:3:64: warning CW-805: \"v2\": interval 50ms "
               "is finer than the tag's refresh 1s",
               "tags/site.yaml:4:102: warning CW-805: \"i1\": interval 200ms "
               "is finer than the tag's refresh 1s"}));
}

// A channel's own poll groups replace the defaults; without
// control/tasks.yaml a virtual tag's `normal` task runs every 100 ms (9.1).
// An internal tag that names no task, and a manual tag, give none.
TEST(TagRules, TheRefreshOfAChannelAndOfTheDefaultTasks) {
    const std::string meter =
        std::string(METER) +
        "  - { name: p1, logging: { policy: sampled, interval: 500ms }, "
        "address: { table: holding, register: 5, type: uint16 } }\n";
    Files files = tags(
        "  - { name: v1, type: virtual, expr: \"1\", logging: { policy: "
        "sampled, interval: 100ms } }\n"
        "  - { name: v2, type: virtual, expr: \"1\", logging: { policy: "
        "sampled, interval: 99ms } }\n"
        // No refresh these rules know: written by a program, or by people.
        "  - { name: i1, type: internal, value_type: int64, logging: { "
        "policy: sampled, interval: 99ms } }\n"
        "  - { name: m1, type: manual, value_type: int64, area: ps1, "
        "logging: { policy: sampled, interval: 99ms } }\n");
    files.emplace("logging.yaml", std::string(LOGGING));
    files.emplace("drivers/modbus/line1.yaml",
                  "channel:\n  name: line1\n  type: rtu\n"
                  "  device: /dev/serial/by-path/platform-serial1\n"
                  "  poll_groups:\n    normal: { period: 500ms, priority: 1 "
                  "}\n"
                  "devices:\n"
                  "  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n");
    EXPECT_EQ(warnings_of(files, meter),
              Lines{"tags/site.yaml:3:81: warning CW-805: \"v2\": interval "
                    "99ms is finer than the tag's refresh 100ms"});
}

// The bits of a point are refreshed with it: a bit's own `logging`, and
// the default policy of a bit given by its name only, are checked (items 1
// and 2 of the review of #184).
TEST(TagRules, TheBitsOfAPoint) {
    Files files;
    files.emplace("logging.yaml",
                  std::string(LOGGING) +
                      "  fast: { mode: periodic, interval: 50ms, "
                      "raw_retention: 1d }\ndefault_policy: fast\n");
    EXPECT_EQ(
        warnings_of(files,
                    "template: meter_a\nprotocol: modbus\nbit_order: lsb0\n"
                    "points:\n"
                    "  - name: status\n"
                    "    address: { table: holding, register: 1, type: uint16 "
                    "}\n"
                    "    tag: false\n"
                    "    bits: { 0: run, 1: { name: trip, logging: { policy: "
                    "sampled, interval: 200ms } } }\n"),
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): long messages
        (Lines{"drivers/modbus/line1.yaml:7:11: warning CW-805: "
               "\"ps1.m1.run\": interval 50ms is finer than the tag's "
               "refresh 1s\n  from template templates/meter_a.yaml:8:16 "
               "(point status)",
               "drivers/modbus/line1.yaml:7:11: warning CW-805: "
               "\"ps1.m1.trip\": interval 200ms is finer than the tag's "
               "refresh 1s\n  from template templates/meter_a.yaml:8:76 "
               "(point status)"}));
}

// A control/tasks.yaml replaces the default tasks (9.1): its `normal` of
// 1 s, not the default 100 ms, is the refresh of a virtual tag of the
// default task (item 1 of the review of #184). A file without `normal`
// leaves such a tag naming no task, which is CF-310 of the references.
TEST(TagRules, ATasksFileReplacesTheDefaults) {
    Files files = tags(
        "  - { name: v1, type: virtual, expr: \"1\", logging: { policy: "
        "sampled, interval: 500ms } }\n");
    files.emplace("logging.yaml", std::string(LOGGING));
    files.emplace("control/tasks.yaml",
                  "tasks:\n  normal: { period: 1s, programs: [] }\n");
    EXPECT_EQ(warnings_of(files),
              Lines{"tags/site.yaml:2:81: warning CW-805: \"v1\": interval "
                    "500ms is finer than the tag's refresh 1s"});
}

// A `wo` point is not refreshed: the `logging` a `defaults` map gives it
// (no CF-614, D-120) gives no CW-805. The read point after it does, once
// for the template's line (D-119).
TEST(TagRules, AWriteOnlyPointHasNoRefresh) {
    Files files;
    files.emplace("logging.yaml",
                  std::string(LOGGING) +
                      "  fast: { mode: periodic, interval: 50ms, "
                      "raw_retention: 1d }\n");
    EXPECT_EQ(
        warnings_of(files,
                    "template: meter_a\nprotocol: modbus\n"
                    "defaults: { logging: fast }\npoints:\n"
                    "  - { name: cmd, access: wo, storage: volatile, address: "
                    "{ table: holding, register: 1, type: uint16 } }\n"
                    "  - { name: flow, address: { table: holding, register: "
                    "2, type: uint16 } }\n"),
        Lines{"drivers/modbus/line1.yaml:7:11: warning CW-805: "
              "\"ps1.m1.flow\": interval 50ms is finer than the tag's "
              "refresh 1s\n  from template templates/meter_a.yaml:3:22 "
              "(point flow)"});
}

// --- Alarms (8) --------------------------------------------------------

// `priorities`, `max_shelve` and `classes` are given in one alarms file
// (CF-811): a later file is refused, naming the first.
TEST(TagRules, TheAlarmListsOfOneFile) {
    EXPECT_EQ(
        errors_of({{"alarms/a.yaml",
                    "priorities: [urgent, high]\nmax_shelve: { high: 1h }\n"},
                   {"alarms/b.yaml", "classes: [process]\n"},
                   {"alarms/c.yaml",
                    "priorities: [high]\nmax_shelve: { high: 2h }\n"
                    "classes: [system]\n"}}),
        (Lines{"alarms/c.yaml:1:13: error CF-811: priorities is given in "
               "\"alarms/a.yaml\" and \"alarms/c.yaml\"",
               "alarms/c.yaml:2:13: error CF-811: max_shelve is given in "
               "\"alarms/a.yaml\" and \"alarms/c.yaml\"",
               "alarms/c.yaml:3:10: error CF-811: classes is given in "
               "\"alarms/b.yaml\" and \"alarms/c.yaml\""}));
}

// The id of an alarm is unique per tag, its type by default (CF-812),
// wherever the alarms are written.
TEST(TagRules, AnIdOnceOnATag) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - name: flow\n"
        "    address: { table: holding, register: 1, type: uint16 }\n"
        "    alarms: [ { type: high, limit: 4, priority: high, area: ps1 } ]\n";
    EXPECT_EQ(
        errors_of({{"alarms/site.yaml",
                    "alarms:\n"
                    "  - { tag: ps1.m1.flow, type: high, limit: 5, priority: "
                    "high, area: ps1 }\n"
                    "  - { tag: ps1.m1.flow, id: other, type: high, limit: 5, "
                    "priority: high, area: ps1 }\n"
                    "  - { tag: ps1.m1.flow, id: other, type: low, limit: 1, "
                    "priority: high, area: ps1 }\n"
                    "  - { tag: ps1.m1.nope, type: high, limit: 5, priority: "
                    "high, area: ps1 }\n"
                    "  - { tag: ps1.m1.nope, type: high, limit: 5, priority: "
                    "high, area: ps1 }\n"}},
                  meter),
        (Lines{"alarms/site.yaml:2:5: error CF-812: tag \"ps1.m1.flow\" has "
               "two alarms with id \"high\"",
               "alarms/site.yaml:4:29: error CF-812: tag \"ps1.m1.flow\" has "
               "two alarms with id \"other\"",
               "alarms/site.yaml:5:12: error CF-310: tag \"ps1.m1.nope\" does "
               "not exist",
               "alarms/site.yaml:6:12: error CF-310: tag \"ps1.m1.nope\" does "
               "not exist"}));
}

// `high_high` above every `high`, `low` above every `low_low`, where both
// are numbers (CF-813); each alarm is compared with the highest of the
// other type.
TEST(TagRules, LimitsInOrder) {
    const auto alarm = [](std::string_view name, std::string_view type,
                          std::string_view limit) {
        return "  - { tag: ps1.m1.flow, id: " + std::string(name) +
               ", type: " + std::string(type) +
               ", limit: " + std::string(limit) +
               ", priority: high, area: ps1 }\n";
    };
    EXPECT_EQ(
        errors_of(
            {{"alarms/site.yaml",
              "alarms:\n" + alarm("h2", "high", "7") +
                  alarm("h1", "high", "5") + alarm("hh1", "high_high", "8") +
                  alarm("hh2", "high_high", "7") +
                  alarm("hh3", "high_high", "6.5") +
                  alarm("hh4", "high_high", "ps1.m1.flow") +
                  alarm("ll1", "low_low", "-1.5") + alarm("l1", "low", "0") +
                  alarm("l2", "low", "-1.5") + alarm("l3", "low", "0x10")}}),
        (Lines{"alarms/site.yaml:5:58: error CF-813: tag \"ps1.m1.flow\": "
               "\"hh2\" must be above \"h2\"",
               "alarms/site.yaml:6:58: error CF-813: tag \"ps1.m1.flow\": "
               "\"hh3\" must be above \"h2\"",
               "alarms/site.yaml:10:51: error CF-813: tag "
               "\"ps1.m1.flow\": \"l2\" must be above \"ll1\""}));
    // Two integer limits are compared exactly, as int64 or uint64 (M2 of
    // the review of #172): 2^53 + 1 is above 2^53, which binary64 does not
    // tell apart.
    EXPECT_EQ(errors_of({{"alarms/site.yaml",
                          "alarms:\n" + alarm("h", "high", "9007199254740992") +
                              alarm("hh", "high_high", "9007199254740993") +
                              alarm("ll", "low_low", "-9223372036854775808") +
                              alarm("l", "low", "-9223372036854775807")}}),
              Lines{});
    EXPECT_EQ(
        errors_of({{"alarms/site.yaml",
                    "alarms:\n" + alarm("h", "high", "18446744073709551614") +
                        alarm("hh", "high_high", "18446744073709551615")}}),
        Lines{});
    EXPECT_EQ(
        errors_of({{"alarms/site.yaml",
                    "alarms:\n" + alarm("h", "high", "18446744073709551615") +
                        alarm("hh", "high_high", "18446744073709551614") +
                        alarm("ll", "low_low", "9223372036854775807") +
                        alarm("l", "low", "-1")}}),
        (Lines{"alarms/site.yaml:3:57: error CF-813: tag \"ps1.m1.flow\": "
               "\"hh\" must be above \"h\"",
               "alarms/site.yaml:5:50: error CF-813: tag \"ps1.m1.flow\": "
               "\"l\" must be above \"ll\""}));
}

// An alarm on a STRING tag is CF-814; a limit alarm on a BOOL tag is
// CF-815, and a state or quality alarm on it is not.
TEST(TagRules, AlarmsOnStringAndBoolTags) {
    EXPECT_EQ(
        errors_of({{"alarms/site.yaml",
                    "alarms:\n"
                    "  - { tag: ps1.m1.label, type: bad_quality, priority: "
                    "high, area: ps1 }\n"
                    "  - { tag: ps1.m1.run, type: low_low, limit: 0, "
                    "priority: high, area: ps1 }\n"
                    "  - { tag: ps1.m1.run, type: state, alarm_on: true, "
                    "priority: high, area: ps1 }\n"
                    "  - { tag: ps1.m1.run, type: bad_quality, priority: "
                    "high, area: ps1 }\n"}}),
        (Lines{"alarms/site.yaml:2:5: error CF-814: string tag "
               "\"ps1.m1.label\" cannot have alarms",
               "alarms/site.yaml:3:30: error CF-815: BOOL tag \"ps1.m1.run\" "
               "cannot have a low_low alarm"}));
}

// A virtual tag's alarm reads its derived type; a template's alarm is
// one error for all its instances.
TEST(TagRules, DerivedTypesAndInstances) {
    const std::string meter =
        std::string(METER) +
        "  - name: on\n"
        "    address: { table: coil, register: 4 }\n"
        "    alarms: [ { type: high, limit: 1, priority: high, area: ps1 } ]\n";
    Files files = tags(
        "  - name: v\n    type: virtual\n"
        "    expr: \"ps1.m1.flow > 1\"\n"
        "    alarms: [ { type: high, limit: 1, priority: high, "
        "area: ps1 } ]\n");
    files.emplace("areas.yaml", std::string(AREAS));
    files.emplace("templates/meter_a.yaml", meter);
    files.emplace(
        "drivers/modbus/line1.yaml",
        channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n"
                "  - { name: ps1.m3, template: meter_a, unit_id: 3 }\n"));
    Lines found;
    for (const std::string& line : expand(files)) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    EXPECT_EQ(found,
              (Lines{"drivers/modbus/line1.yaml:7:13: error CF-815: BOOL tag "
                     "\"ps1.m1.on\" cannot have a high alarm (and 2 more "
                     "instances: ps1.m2, ps1.m3)\n  from template "
                     "templates/meter_a.yaml:9:23 (point on)",
                     "tags/site.yaml:5:23: error CF-815: BOOL tag \"v\" "
                     "cannot have a high alarm"}));
}

// --- The review of #172 ------------------------------------------------

// S4: the alarms and keys of a bit given as a map are checked; a `tag:
// false` point has no keys of its own to check, but its alarms that name a
// tag are.
TEST(TagRules, BitsAndPointsWithoutATag) {
    const std::string meter =
        std::string(METER) +
        "  - name: status\n"
        "    address: { table: holding, register: 5, type: uint16 }\n"
        "    tag: false\n"
        "    retain_max_age: 1h\n"
        "    bits: { 0: { name: hot, alarms: [ { type: high, limit: 1, "
        "priority: high, area: ps1 } ] } }\n"
        "    alarms: [ { tag: ps1.m1.run, type: low, limit: 0, priority: high, "
        "area: ps1 } ]\n"
        "bit_order: lsb0\n";
    EXPECT_EQ(errors_of({}, meter),
              (Lines{"drivers/modbus/line1.yaml:7:11: error CF-815: BOOL tag "
                     "\"ps1.m1.run\" cannot have a low alarm\n  from template "
                     "templates/meter_a.yaml:12:40 (point status)",
                     "drivers/modbus/line1.yaml:7:11: error CF-815: BOOL tag "
                     "\"ps1.m1.hot\" cannot have a high alarm\n  from "
                     "template templates/meter_a.yaml:11:47 (point status)"}));
}

// S4: a device given twice (CF-303) is checked once: the alarms of its
// `tag: false` point are not two alarms of one id on a tag (CF-812). Its
// tags' own keys are skipped by S1 already.
TEST(TagRules, ADeviceGivenTwiceIsCheckedOnce) {
    const std::string meter =
        std::string(METER) +
        "  - { name: total, retain_max_age: 1h, address: { table: holding, "
        "register: 5, type: uint32 } }\n"
        "  - { name: word, tag: false, address: { table: holding, register: "
        "7, type: uint16 }, alarms: [ { tag: ps1.m1.flow, type: high, limit: "
        "4, priority: high, area: ps1 } ] }\n";
    const Files files{
        {"areas.yaml", std::string(AREAS)},
        {"templates/meter_a.yaml", meter},
        {"drivers/modbus/line1.yaml",
         channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                 "  - { name: ps1.m1, template: meter_a, unit_id: 2 }\n")}};
    Lines found;
    for (const std::string& line : expand(files)) {
        if (line.find(": error ") != std::string::npos) {
            found.push_back(line);
        }
    }
    EXPECT_EQ(found,
              (Lines{"drivers/modbus/line1.yaml:7:13: error CF-506: "
                     "retain_max_age needs retain: true\n  from template "
                     "templates/meter_a.yaml:7:36 (point total)",
                     "drivers/modbus/line1.yaml:8:13: error CF-303: device "
                     "\"ps1.m1\" is defined twice; first at "
                     "drivers/modbus/line1.yaml:7"}));
}

// S1: the second definition of a name (CF-302) is not checked against the
// first one's type: a float64 with a high alarm after a bool of its name
// gives no CF-815.
TEST(TagRules, TheSecondOfTwoDefinitionsIsNotChecked) {
    EXPECT_EQ(errors_of(tags(
                  "  - { name: t, type: internal, value_type: bool }\n"
                  "  - name: t\n    type: internal\n    value_type: float64\n"
                  "    alarms: [ { type: high, limit: 1, priority: high, "
                  "area: ps1 } ]\n")),
              Lines{"tags/site.yaml:3:11: error CF-302: tag \"t\" is defined "
                    "twice; first at tags/site.yaml:2"});
}

// S2 and S4: past CF-1001, a device the tags did not record (the limit
// stops the collector within ps1.m1) is still checked once; no CF-505 is
// told of an alarm, whose tag may be one that does not exist; and no rule
// reads a type, also of a tag stored before the limit (the string point
// `label`, retained, in the next test).
TEST(TagRules, PastTheTagLimitEveryDeviceIsChecked) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - { name: flow, range: [0, 10], address: { table: holding, "
        "register: 1, type: uint16 } }\n"
        "  - { name: label, retain: true, address: { table: holding, "
        "register: 3, type: string, length: 4 } }\n"
        "  - { name: total, retain_max_age: 1h, address: { table: holding, "
        "register: 5, type: uint32 } }\n";
    const Files files{
        {"areas.yaml", std::string(AREAS)},
        {"templates/meter_a.yaml", meter},
        {"drivers/modbus/line1.yaml",
         channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                 "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n")},
        {"system.yaml", std::string(SYSTEM) + "limits: { max_tags: 40 }\n"},
        {"alarms/site.yaml",
         "alarms:\n  - { tag: ps1.m2.flow, type: high, limit: 4, deadband: "
         "1%, priority: high, area: ps1 }\n  - { tag: ps1.m9.flow, type: high, "
         "limit: 4, deadband: 1%, priority: high, area: ps1 }\n"}};
    Lines found;
    for (const std::string& line : expand(files)) {
        if (line.find(": error ") != std::string::npos &&
            line.find("CF-1001") == std::string::npos) {
            found.push_back(line);
        }
    }
    EXPECT_EQ(found,
              Lines{"drivers/modbus/line1.yaml:7:13: error CF-506: "
                    "retain_max_age needs retain: true (and 1 more instance: "
                    "ps1.m2)\n  from template templates/meter_a.yaml:6:36 "
                    "(point total)"});
}

TEST(TagRules, PastTheTagLimitNoTypeIsRead) {
    const std::string meter =
        "template: meter_a\nprotocol: modbus\npoints:\n"
        "  - { name: flow, range: [0, 10], address: { table: holding, "
        "register: 1, type: uint16 } }\n"
        "  - { name: label, retain: true, address: { table: holding, "
        "register: 3, type: string, length: 4 } }\n"
        "  - { name: total, retain_max_age: 1h, address: { table: holding, "
        "register: 5, type: uint32 } }\n";
    const Files files{
        {"areas.yaml", std::string(AREAS)},
        {"templates/meter_a.yaml", meter},
        {"drivers/modbus/line1.yaml",
         channel("  - { name: ps1.m1, template: meter_a, unit_id: 1 }\n"
                 "  - { name: ps1.m2, template: meter_a, unit_id: 2 }\n")},
        {"system.yaml", std::string(SYSTEM) + "limits: { max_tags: 50 }\n"},
        {"alarms/site.yaml",
         "alarms:\n  - { tag: ps1.m2.flow, type: high, limit: 4, deadband: "
         "1%, priority: high, area: ps1 }\n  - { tag: ps1.m9.flow, type: high, "
         "limit: 4, deadband: 1%, priority: high, area: ps1 }\n"}};
    Lines found;
    for (const std::string& line : expand(files)) {
        if (line.find(": error ") != std::string::npos &&
            line.find("CF-1001") == std::string::npos) {
            found.push_back(line);
        }
    }
    // At 50 tags `ps1.m1.label` is stored before the limit: its CF-507
    // must still not be told (the `complete()` guard of `value_type`).
    for (const std::string& line : found) {
        EXPECT_EQ(line.find("CF-507"), std::string::npos) << line;
    }
}

// S4: a tier whose interval stage 3 refused is not compared, nor the one
// after it with it. Validation never reaches stage 5 with one, so the
// check is called on its own here.
TEST(TagRules, ARefusedTierIsNotCompared) {
    Diagnostics parsed;
    std::optional<Document> logging = parse_yaml(
        "aggregate_tiers:\n  - { name: a, interval: 1m }\n"
        "  - { name: b, interval: soon }\n  - { name: c, interval: 90s }\n",
        "logging.yaml", parsed);
    std::optional<Document> empty = parse_yaml("{}\n", "", parsed);
    if (!logging || !empty) {
        FAIL() << "not parsed";
    }
    const ScalarTypes types(*logging);
    const TreeFile source{.path = "logging.yaml",
                          .path_class = PathClass{.kind = PathClass::Kind::FILE,
                                                  .type = FileType::LOGGING,
                                                  .protocol = {},
                                                  .stem = "logging"},
                          .content = {}};
    const std::array<CheckedFile, 1> files{
        CheckedFile{.source = &source, .document = &*logging, .types = &types}};
    const ExpandedTree tree{.document = std::move(*empty),
                            .types = {},
                            .instance_of = {},
                            .instances = {},
                            .computed = {},
                            .from_defaults = {},
                            .max_tags = ExpandedTree::DEFAULT_MAX_TAGS};
    Diagnostics diagnostics;
    check_tag_rules(TagNames{}, tree, files, diagnostics);
    EXPECT_EQ(diagnostics.error_count(), 0U);
}

}  // namespace
}  // namespace scada::conf
