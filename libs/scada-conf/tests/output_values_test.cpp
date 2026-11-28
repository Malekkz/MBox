// The values an output writes against its target, and the fail-safe
// defaults (configuration schemas contract 2.3, 7.2, 7.4; decision D-124,
// rulings Q7 and Q8 of slice 7e-1 of #100).

#include "output_values.h"
#include "output_rules.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>
#include <tuple>

namespace scada::conf {
namespace {

// Compares are by mathematical value, whatever the written form.
TEST(OutputValues, NumbersCompareByValue) {
    EXPECT_GT(compare_numbers("10", "9").value_or(0), 0);
    EXPECT_LT(compare_numbers("9", "10").value_or(0), 0);
    EXPECT_GT(compare_numbers("1e2", "99.5").value_or(0), 0);
    EXPECT_LT(compare_numbers("-1.5", "-1").value_or(0), 0);
    EXPECT_EQ(compare_numbers("0.10", "0.1"), 0);
    EXPECT_EQ(compare_numbers("0x10", "16"), 0);
    EXPECT_EQ(compare_numbers("-0", "0"), 0);
    EXPECT_LT(compare_numbers("6553.5", "6553.55").value_or(0), 0);
    EXPECT_EQ(compare_numbers("x", "1"), std::nullopt);
}

// Q8: exactly by value, no casts; interim rule (f) for FLOAT64.
TEST(OutputValues, ValueTypes) {
    EXPECT_TRUE(fits_value_type("9223372036854775807", "int64"));
    EXPECT_FALSE(fits_value_type("9223372036854775808", "int64"));
    EXPECT_TRUE(fits_value_type("-9223372036854775808", "int64"));
    EXPECT_FALSE(fits_value_type("-9223372036854775809", "int64"));
    EXPECT_FALSE(fits_value_type("1.5", "int64"));
    EXPECT_TRUE(fits_value_type("2.0", "int64"));
    EXPECT_TRUE(fits_value_type("1e3", "int64"));
    EXPECT_FALSE(fits_value_type("-1", "uint64"));
    EXPECT_TRUE(fits_value_type("-0", "uint64"));
    EXPECT_TRUE(fits_value_type("18446744073709551615", "uint64"));
    EXPECT_FALSE(fits_value_type("18446744073709551616", "uint64"));
    EXPECT_TRUE(fits_value_type("9007199254740992", "float64"));
    EXPECT_FALSE(fits_value_type("9007199254740993", "float64"));
    EXPECT_TRUE(fits_value_type("18014398509481988", "float64"));
    EXPECT_TRUE(fits_value_type("0.1", "float64"));
    EXPECT_TRUE(fits_value_type("true", "bool"));
    EXPECT_FALSE(fits_value_type("1", "bool"));
    EXPECT_FALSE(fits_value_type("true", "int64"));
}

TEST(OutputValues, RawWidths) {
    EXPECT_TRUE(fits_raw_width("-128", "int8"));
    EXPECT_FALSE(fits_raw_width("-129", "int8"));
    EXPECT_TRUE(fits_raw_width("255", "uint8"));
    EXPECT_FALSE(fits_raw_width("256", "uint8"));
    EXPECT_TRUE(fits_raw_width("0xFFFF", "uint16"));
    EXPECT_TRUE(fits_raw_width("-32768", "int16"));
    EXPECT_FALSE(fits_raw_width("-32769", "int16"));
    EXPECT_TRUE(fits_raw_width("1", "bool"));
    EXPECT_FALSE(fits_raw_width("2", "bool"));
    EXPECT_FALSE(fits_raw_width("1", "string"));
}

Scaling with_gain(std::string_view gain) {
    return {.gain = gain, .offset = {}, .divide_by = {}};
}
Scaling with_offset(std::string_view offset) {
    return {.gain = {}, .offset = offset, .divide_by = {}};
}
Scaling with_divide_by(std::string_view divide_by) {
    return {.gain = {}, .offset = {}, .divide_by = divide_by};
}
Scaling none() {
    return {.gain = {}, .offset = {}, .divide_by = {}};
}

// The inverse scaling of 7.2: exact on the integer path, else binary64
// rounded half away from zero.
TEST(OutputValues, TheInverseScaling) {
    // Exact: (v - offset) / gain must divide.
    EXPECT_TRUE(survives_scaling("10", "int64", "int16", with_gain("2")));
    EXPECT_FALSE(survives_scaling("11", "int64", "int16", with_gain("2")));
    EXPECT_TRUE(survives_scaling("-32767", "int64", "int16", with_offset("1")));
    EXPECT_FALSE(
        survives_scaling("-32768", "int64", "int16", with_offset("1")));
    EXPECT_FALSE(survives_scaling("70000", "int64", "uint16", none()));
    // binary64: 6553.55 x 10 is 65535.5, which rounds to 65536.
    EXPECT_TRUE(
        survives_scaling("6553.5", "float64", "uint16", with_divide_by("10")));
    EXPECT_FALSE(
        survives_scaling("6553.55", "float64", "uint16", with_divide_by("10")));
    EXPECT_FALSE(survives_scaling("-0.5", "float64", "uint16", none()));
    EXPECT_TRUE(survives_scaling("-0.4", "float64", "uint16", none()));
    EXPECT_FALSE(survives_scaling("1e40", "float64", "float32", none()));
    EXPECT_TRUE(
        survives_scaling("12.5", "float64", "uint16", with_gain("0.5")));
}

// Q7: the defaults, for each kind, protocol and safe state.
TEST(OutputValues, TheFailSafeDefaults) {
    using Row =
        std::tuple<std::string_view, std::string_view, bool, std::string_view,
                   std::string_view, std::string_view, std::string_view>;
    // kind, protocol, dangerous, safe_state -> on_shutdown, on_control_loss,
    // after.
    for (const auto& [kind, protocol, dangerous, state, shutdown, loss, after] :
         {Row{"single", "io", false, "off", "safe", "safe", "0s"},
          Row{"single", "io", false, "hold", "safe", "safe", "0s"},
          Row{"setpoint", "io", false, "", "safe", "safe", "0s"},
          Row{"pulse", "io", false, "hold", "hold", "hold", ""},
          Row{"step", "io", false, "hold", "hold", "hold", ""},
          Row{"trigger", "io", false, "hold", "hold", "hold", ""},
          Row{"single", "io", true, "hold", "hold", "hold", ""},
          Row{"single", "modbus", false, "off", "hold", "safe", "30s"},
          Row{"double", "modbus", false, "on", "hold", "safe", "30s"},
          Row{"double", "modbus", false, "hold", "hold", "hold", ""},
          Row{"single", "modbus", true, "off", "hold", "hold", ""},
          Row{"setpoint", "modbus", false, "", "hold", "hold", ""},
          Row{"pulse", "modbus", false, "hold", "hold", "hold", ""}}) {
        const FailSafeDefaults defaults =
            fail_safe_defaults(kind, protocol, dangerous, state);
        EXPECT_EQ(defaults.on_shutdown, shutdown) << kind << " " << protocol;
        EXPECT_EQ(defaults.on_control_loss, loss) << kind << " " << protocol;
        EXPECT_EQ(defaults.control_loss_after, after)
            << kind << " " << protocol;
        EXPECT_EQ(defaults.on_generation_change, "hold");
    }
}

}  // namespace
}  // namespace scada::conf
