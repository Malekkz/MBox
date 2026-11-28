#ifndef SCADA_CONF_SRC_OUTPUT_VALUES_H
#define SCADA_CONF_SRC_OUTPUT_VALUES_H

// The values an output writes, against its target's types (configuration
// schemas contract 2.3 and 7.2; CF-713; decision D-124, ruling Q8 of the
// review of slice 7e-1 of #100). Every compare is by mathematical value:
// a number is read from its text, never cast.

#include <optional>
#include <string_view>

namespace scada::conf {

// The scaling of a point (4.7): each text as written, empty when absent.
struct Scaling {
    std::string_view gain;
    std::string_view offset;
    std::string_view divide_by;
};

// True if the number written as `value` (an integer, hex included, or a
// decimal) is a value of `value_type` (`int64`, `uint64`, `float64`):
// an integer of the type's range, exactly; for `float64` any decimal, and
// an integer that binary64 holds exactly (interim rule (f) of D-124). A
// `bool` type takes `true` or `false`, which no other type takes.
[[nodiscard]] bool fits_value_type(std::string_view value,
                                   std::string_view value_type);

// The order of two numbers written as texts, by mathematical value:
// negative, zero or positive as `lhs` is below, equal to or above `rhs`;
// nothing when either is not a number.
[[nodiscard]] std::optional<int> compare_numbers(std::string_view lhs,
                                                 std::string_view rhs);

// True if the raw pattern `pattern` (an integer, hex included) fits the
// width of raw type `raw` (2.3): 0 to 2^w - 1, or a negative value of at
// least -2^(w-1). A raw type with no width (`string`, `bytes`) holds none.
[[nodiscard]] bool fits_raw_width(std::string_view pattern,
                                  std::string_view raw);

// True if the engineering value `value` of a target of `value_type`
// survives the inverse scaling of 7.2 into raw type `raw`: the exact
// integer path for an `int64` or `uint64` target whose scaling is all
// integers, else binary64, rounded half away from zero for an integer raw
// type; the raw value must be finite and of the raw type's range. A raw
// type this cannot count (`mod10k`, `epoch`, `datetime`, `string`, ...) is
// not checked here: CF-708 refuses writing it.
[[nodiscard]] bool survives_scaling(std::string_view value,
                                    std::string_view value_type,
                                    std::string_view raw,
                                    const Scaling& scaling);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_OUTPUT_VALUES_H
