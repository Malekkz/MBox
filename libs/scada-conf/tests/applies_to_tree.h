#ifndef SCADA_CONF_TESTS_APPLIES_TO_TREE_H
#define SCADA_CONF_TESTS_APPLIES_TO_TREE_H

// The tree of the limits of `applies_to` patterns for CF-311 (D-116; #176),
// shared by the unit test of the budget and its performance test. Invented
// examples only.

#include <cstddef>
#include <string>

namespace scada::conf {

// meter_a with `points` points named `x`, 59 `a` and a number, and `rules`
// health rules whose pattern `*` + 40 `a` + `b` matches none of them, each
// after about 2,600 matcher steps per name, so each checked one is CF-311.
// After them, a rule that names the point `nope`, which does not exist
// (CF-310, not budgeted), and one with the pattern `q*`, which matches none
// (CF-311 only when the budget is not spent).
inline std::string applies_to_meter(std::size_t points, std::size_t rules) {
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
    meter += "health:\n";
    const std::string pattern = "\"*" + std::string(PATTERN, 'a') + "b\"";
    const std::string rule = "  - { point: " + stem +
                             "0, good_when: 0, applies_to: [ " + pattern +
                             " ] }\n";
    for (std::size_t i = 0; i < rules; ++i) {
        meter += rule;
    }
    meter += "  - { point: " + stem +
             "0, good_when: 0, applies_to: [ nope ] }\n"
             "  - { point: " +
             stem + "0, good_when: 0, applies_to: [ \"q*\" ] }\n";
    return meter;
}

// meter_a with `points` points `p0` to `p<n-1>` and one health rule that
// lists `points` patterns `p*`, each matched at the first name visited: the
// whole cost is the visits, every one counted (the delta check of #174,
// B-D1). The tree is valid.
inline std::string cheap_patterns_meter(std::size_t points) {
    std::string meter = "template: meter_a\nprotocol: modbus\npoints:\n";
    for (std::size_t i = 0; i < points; ++i) {
        meter +=
            "  - { name: p" + std::to_string(i) +
            ", address: { table: holding, register: " + std::to_string(i + 1) +
            ", type: uint16 } }\n";
    }
    meter += "health:\n  - point: p0\n    good_when: 0\n    applies_to:\n";
    for (std::size_t i = 0; i < points; ++i) {
        meter += "      - \"p*\"\n";
    }
    return meter;
}

}  // namespace scada::conf

#endif  // SCADA_CONF_TESTS_APPLIES_TO_TREE_H
