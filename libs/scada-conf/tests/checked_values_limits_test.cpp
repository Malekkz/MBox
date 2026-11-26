// The bound of the results of computed values kept (D-115): 65,537 values
// checked, so it has its own executable and time limit, apart from the
// point limit (scada-conf-limits-test) and the unit tests of the checks of
// a merged point.

#include "expand_cases.h"

#include <gtest/gtest.h>

#include <string>

namespace scada::conf {
namespace {

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
