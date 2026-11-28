#ifndef SCADA_DRIVER_SRC_ELEMENT_H
#define SCADA_DRIVER_SRC_ELEMENT_H

// The checked accessor of `std::array` for noexcept code, where `.at` would
// throw (coding standard 3 and 7; PR 177 S5): an index out of range is a
// programmer's error and ends the process.

#include "scada/common/panic.h"
#include "scada/common/realtime.h"

#include <array>
#include <cstddef>
#include <span>

namespace scada::driver::detail {

// `items[index]`; ends the process when `index` is not below N.
template <class T, std::size_t N>
[[nodiscard]] SCADA_RT T& element(std::array<T, N>& items,
                                  std::size_t index) noexcept {
    if (index >= N) {
        panic("an array index out of range");
    }
    return std::span<T, N>(items)[index];
}

template <class T, std::size_t N>
[[nodiscard]] SCADA_RT const T& element(const std::array<T, N>& items,
                                        std::size_t index) noexcept {
    if (index >= N) {
        panic("an array index out of range");
    }
    return std::span<const T, N>(items)[index];
}

}  // namespace scada::driver::detail

#endif  // SCADA_DRIVER_SRC_ELEMENT_H
