// The checked accessor of std::array for noexcept code (element.h; issue
// #181): an index in range gives the element. One out of range ends the
// process through `panic`; that path has no unit test, since the project's
// checks ban the `goto` a gtest death test expands to.

#include "../src/element.h"

#include <gtest/gtest.h>

#include <array>

namespace scada::driver::detail {
namespace {

TEST(Element, AnIndexInRangeGivesTheElement) {
    constexpr int WRITTEN = 7;
    std::array<int, 3> items{1, 2, 3};
    element(items, 2) = WRITTEN;
    EXPECT_EQ(items[2], WRITTEN);
    const std::array<int, 3>& view = items;
    EXPECT_EQ(element(view, 0), 1);
}

}  // namespace
}  // namespace scada::driver::detail
