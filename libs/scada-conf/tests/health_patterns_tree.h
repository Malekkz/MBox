#ifndef SCADA_CONF_TESTS_HEALTH_PATTERNS_TREE_H
#define SCADA_CONF_TESTS_HEALTH_PATTERNS_TREE_H

// The tree of the limits of health rules' patterns (D-120; M4 of the review
// of #174), shared by the unit test of the budget and its performance test.
// Invented examples only.

#include <cstddef>
#include <string>

namespace scada::conf {

// meter_a with `points` points named `x`, 59 `a` and a number, and `rules`
// health rules whose pattern `*` + 40 `a` + `b` matches none of them, each
// after about 2,600 steps per name. Only `w` + 40 `a` + `b` matches, a point
// in the health point's group: it serves, so it is not compared, and as the
// first name it keeps CF-311 (D-116, #176) cheap. After them, a rule that
// lists the name `zz` and one with the pattern `z*`, both giving CF-612 when
// checked: the first because a name is not budgeted, the second only before
// the budget runs out.
inline std::string health_patterns_meter(std::size_t points,
                                         std::size_t rules) {
    constexpr std::size_t STEM = 59;     // `x` and 59 `a`
    constexpr std::size_t PATTERN = 40;  // `*`, 40 `a`, `b`
    const std::string stem = "x" + std::string(STEM, 'a');
    std::string meter = "template: meter_a\nprotocol: modbus\npoints:\n";
    for (std::size_t i = 0; i < points; ++i) {
        meter +=
            "  - { name: " + stem + std::to_string(i) +
            ", address: { table: holding, register: " + std::to_string(i + 1) +
            ", type: uint16 } }\n";
    }
    meter += "  - { name: w" + std::string(PATTERN, 'a') +
             "b, poll_group: fast, address: { table: holding, register: "
             "60000, type: uint16 } }\n"
             "  - { name: h, poll_group: fast, address: { table: holding, "
             "register: 60001, type: uint16 } }\n"
             "  - { name: zz, address: { table: holding, register: 60002, "
             "type: uint16 } }\n"
             "health:\n";
    const std::string pattern = "\"*" + std::string(PATTERN, 'a') + "b\"";
    for (std::size_t i = 0; i < rules; ++i) {
        meter +=
            "  - { point: h, good_when: 0, applies_to: [ " + pattern + " ] }\n";
    }
    meter +=
        "  - { point: h, good_when: 0, applies_to: [ zz ] }\n"
        "  - { point: h, good_when: 0, applies_to: [ \"z*\" ] }\n";
    return meter;
}

// meter_a with `points` points `p0` to `p<n-1>` and the health point `h`,
// all in `normal`, and one health rule that lists `points` patterns `p*`:
// every point is served, so no pattern needs a match (B-D1 of the delta
// check of #174). The tree is valid.
inline std::string served_patterns_meter(std::size_t points) {
    std::string meter = "template: meter_a\nprotocol: modbus\npoints:\n";
    for (std::size_t i = 0; i < points; ++i) {
        meter += "  - { name: p" + std::to_string(i) +
                 ", address: { table: holding, register: " +
                 std::to_string(i + 1) + ", type: uint16 } }\n";
    }
    meter +=
        "  - { name: h, address: { table: holding, register: 60001, type: "
        "uint16 } }\n"
        "health:\n  - point: h\n    good_when: 0\n    applies_to:\n";
    for (std::size_t i = 0; i < points; ++i) {
        meter += "      - \"p*\"\n";
    }
    return meter;
}

// The CF-612 of the `zz` rule of `health_patterns_meter(points, rules)`.
inline std::string health_patterns_zz_error(std::size_t points,
                                            std::size_t rules) {
    // The header, the points, `w`, `h`, `zz`, `health:` and the rules.
    constexpr std::size_t LINES_BEFORE = 8;
    return "drivers/modbus/line1.yaml:7:11: error CF-612: point \"zz\": "
           "\"h\" must be in poll group \"normal\"\n  from template "
           "templates/meter_a.yaml:" +
           std::to_string(points + rules + LINES_BEFORE) + ":14 (point zz)";
}

}  // namespace scada::conf

#endif  // SCADA_CONF_TESTS_HEALTH_PATTERNS_TREE_H
