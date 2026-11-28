// The point limit of an instance (CF-414, contract 1.3; decision D-114) at
// N and N + 1: 131,074 points made, so it has its own executable and time
// limit, apart from the unit tests of the expander.

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace scada::conf {
namespace {

// 65,536 points are valid; one more is CF-414, on the points of the
// instance after step 4. The nodes made before it are bounded by the node
// limit of the tree (TheLimitsOfTheExpandedTree).
TEST(Expand, ThePointLimit) {
    const auto with = [](std::string_view range) {
        return expand(
            {{"templates/meter_a.yaml",
              "template: meter_a\nprotocol: modbus\n"
              "repeat:\n"
              "  - for: { n: \"" +
                  std::string(range) +
                  "\" }\n"
                  "    points:\n"
                  // Registers 0 to 32,767 and 32,768 to 65,535 (CF-602,
                  // D-122).
                  "      - { name: \"a{n}\", address: { table: holding, "
                  "register: \"n - 1\", type: uint16 } }\n"
                  "      - { name: \"b{n}\", address: { table: holding, "
                  "register: \"n + 32767\", type: uint16 } }\n"},
             {"drivers/modbus/line1.yaml", one_device()},
             {"system.yaml", std::string(SYSTEM) + "limits: { max_tags: "
                                                   "1000000 }\n"}});
    };
    EXPECT_EQ(with("1..32768").size(), 65536U);
    // CF-414 counts the points that exist after step 4, of the instance
    // (C2 of the review of #146).
    EXPECT_EQ(with("1..32769"),
              Lines{"drivers/modbus/line1.yaml:7:11: error CF-414: template "
                    "\"meter_a\" expands to 65538 points, the limit is "
                    "65536"});
}

}  // namespace
}  // namespace scada::conf
