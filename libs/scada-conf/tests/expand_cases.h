#ifndef SCADA_CONF_TESTS_EXPAND_CASES_H
#define SCADA_CONF_TESTS_EXPAND_CASES_H

// Trees for the tests of stage 4: a tree of files validated and expanded,
// as `scada-conf` does, and the channel and device most tests use. Shared by
// `expand_test.cpp` and `limits_test.cpp`.

#include "expanded_text.h"
#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"
#include "scada/test/temp_directory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

namespace fs = std::filesystem;

using Lines = std::vector<std::string>;
using Files = std::map<std::string, std::string>;

inline constexpr std::string_view SYSTEM =
    "unit: { id: site-0042, name: Test unit }\n"
    "time: { timezone: Etc/UTC, sync: { mode: none } }\n";

// Validates and expands the tree of `files` with a system.yaml: its errors
// in the form of contract 1.4 (with their notes), or its expanded points,
// with each device's keys first when `with_devices`.
inline Lines expand(const Files& files, bool with_devices = false) {
    std::optional<test::TempDirectory> temp =
        test::TempDirectory::create("scada-conf-expand-");
    EXPECT_TRUE(temp.has_value());
    if (!temp) {
        return {"no temporary directory"};
    }
    Files all = files;
    all.try_emplace("system.yaml", SYSTEM);
    for (const auto& [path, content] : all) {
        const fs::path full = temp->path() / path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
        EXPECT_TRUE(out.good());
    }
    const Result<Validation> validation = validate_and_expand(temp->path());
    EXPECT_TRUE(validation.ok());
    if (!validation.ok()) {
        return {"not validated"};
    }
    const Validation& result = validation.value();
    if (!result.report.errors.empty()) {
        Lines lines;
        for (const Diagnostic& error : result.report.errors) {
            lines.push_back(format(error));
        }
        return lines;
    }
    if (!result.expanded) {
        return {"no expanded tree"};
    }
    return points_of(*result.expanded, with_devices);
}

// A Modbus RTU channel holding the devices given.
inline std::string channel(std::string_view devices) {
    return "channel:\n"
           "  name: line1\n"
           "  type: rtu\n"
           "  device: /dev/serial/by-path/platform-serial1\n"
           "  baud: 9600\n"
           "devices:\n" +
           std::string(devices);
}

// One device `ps1.m1` of template `meter_a`, with more keys after it.
inline std::string one_device(std::string_view more = {}) {
    return channel(
        "  - name: ps1.m1\n"
        "    template: meter_a\n"
        "    unit_id: 1\n" +
        std::string(more));
}

}  // namespace scada::conf

#endif  // SCADA_CONF_TESTS_EXPAND_CASES_H
