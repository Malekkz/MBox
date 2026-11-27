// Stage 5: retained values, logging and alarms of the tags (configuration
// schemas contract 5.2, 6 and 8; decision D-119; slice 7c of #100).

#include "expand_cases.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

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

// The warnings of the tree of `files`, which has no error.
Lines warnings_of(Files files, std::string_view meter = METER,
                  std::string_view devices = {}) {
    files.try_emplace("areas.yaml", std::string(AREAS));
    files.try_emplace("templates/meter_a.yaml", std::string(meter));
    files.try_emplace("drivers/modbus/line1.yaml",
                      devices.empty() ? one_device() : channel(devices));
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
            "retain_max_age: 1h }\n")),
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
               "allowed on counter tags"}));
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
// percentage deadband of its policy needs a range like any other (CF-505).
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
               "tags/site.yaml:6:62: error CF-505: \"s3\": a percentage "
               "needs range",
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

// A percentage deadband needs the tag's `range` (CF-505): at the tag's own
// deadband, or at the `logging` that takes it from a policy.
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
        "area: ps1 } ]\n");
    files.emplace("logging.yaml", std::string(LOGGING));
    files.emplace("alarms/site.yaml",
                  "alarms:\n  - { tag: ps1.m1.flow, type: high, limit: 4, "
                  "deadband: 1%, priority: high, area: ps1 }\n");
    EXPECT_EQ(
        errors_of(files),
        (Lines{"alarms/site.yaml:2:57: error CF-505: \"ps1.m1.flow\": a "
               "percentage needs range",
               "tags/site.yaml:2:62: error CF-505: \"a\": a percentage needs "
               "range",
               "tags/site.yaml:4:91: error CF-505: \"c\": a percentage needs "
               "range",
               "tags/site.yaml:9:49: error CF-505: \"e\": a percentage needs "
               "range"}));
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
              "alarms:\n" + alarm("h1", "high", "5") +
                  alarm("h2", "high", "7") + alarm("hh1", "high_high", "8") +
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

}  // namespace
}  // namespace scada::conf
