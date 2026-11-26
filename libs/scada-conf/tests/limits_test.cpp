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
                  "      - { name: \"a{n}\", address: { table: holding, "
                  "register: 1, type: uint16 } }\n"
                  "      - { name: \"b{n}\", address: { table: holding, "
                  "register: 2, type: uint16 } }\n"},
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

// The results of computed values kept are at most 65,536 (D-115, S5 of the
// review of #149): past the bound they are checked again, with the same
// errors. ps1.m1 and ps1.m3 compute two registers out of range from one
// template line (one cause); ps1.m2, between them, computes 65,535 values
// that fill the bound.
TEST(Expand, TheBoundOfTheCheckedValues) {
    const std::string wrong =
        "template: wrong\nprotocol: modbus\nrepeat:\n"
        "  - for: { n: \"1..2\" }\n    points:\n"
        "      - { name: \"w{n}\", address: { table: holding, register: \"n "
        "+ 70000\", type: uint16 } }\n";
    const std::string many =
        "template: many\nprotocol: modbus\nrepeat:\n"
        "  - for: { n: \"1..65535\" }\n    points:\n"
        "      - { name: \"m{n}\", address: { table: holding, register: "
        "\"n\", type: uint16 } }\n";
    EXPECT_EQ(
        expand({{"templates/wrong.yaml", wrong},
                {"templates/many.yaml", many},
                {"drivers/modbus/line1.yaml",
                 channel("  - { name: ps1.m1, template: wrong, unit_id: 1 }\n"
                         "  - { name: ps1.m2, template: many, unit_id: 2 }\n"
                         "  - { name: ps1.m3, template: wrong, unit_id: 3 "
                         "}\n")},
                {"system.yaml", std::string(SYSTEM) + "limits: { max_tags: "
                                                      "1000000 }\n"}}),
        Lines{"drivers/modbus/line1.yaml:7:13: error CF-204: "
              "address.register must be between 0 and 65536 (and 1 more "
              "instance: ps1.m3)\n  from template templates/wrong.yaml:6:62 "
              "(point w1)"});
}

}  // namespace
}  // namespace scada::conf
