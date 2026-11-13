// Unit tests for the owning file descriptor.

#include "scada/common/fd.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <type_traits>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using scada::Fd;

static_assert(!std::is_copy_constructible_v<Fd>);
static_assert(!std::is_copy_assignable_v<Fd>);
static_assert(std::is_nothrow_move_constructible_v<Fd>);
static_assert(std::is_nothrow_move_assignable_v<Fd>);

bool is_open(int descriptor) {
    struct stat info {};
    return ::fstat(descriptor, &info) == 0 || errno != EBADF;
}

// Returns both ends of a new pipe, or -1 twice.
std::array<int, 2> make_pipe() {
    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0) {
        ends = {-1, -1};
    }
    return ends;
}

TEST(Fd, DefaultHoldsNothing) {
    const Fd empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(empty.get(), -1);
}

TEST(Fd, NegativeDescriptorGivesAnInvalidFd) {
    const Fd failed(-1);
    EXPECT_FALSE(failed.valid());
    EXPECT_EQ(failed.get(), -1);
}

TEST(Fd, DestructorCloses) {
    const std::array<int, 2> ends = make_pipe();
    ASSERT_GE(ends[0], 0);
    {
        const Fd reader(ends[0]);
        const Fd writer(ends[1]);
        EXPECT_TRUE(reader.valid());
        EXPECT_EQ(reader.get(), ends[0]);
        EXPECT_TRUE(is_open(ends[0]));
    }
    EXPECT_FALSE(is_open(ends[0]));
    EXPECT_FALSE(is_open(ends[1]));
}

TEST(Fd, MoveConstructionTransfers) {
    const std::array<int, 2> ends = make_pipe();
    ASSERT_GE(ends[0], 0);
    Fd first(ends[0]);
    const Fd second(ends[1]);

    const Fd moved(std::move(first));
    EXPECT_EQ(moved.get(), ends[0]);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the moved-from state is what is tested
    EXPECT_FALSE(first.valid());
}

TEST(Fd, MoveAssignmentClosesThePreviousDescriptor) {
    const std::array<int, 2> ends = make_pipe();
    ASSERT_GE(ends[0], 0);
    Fd target(ends[0]);
    Fd source(ends[1]);

    target = std::move(source);
    EXPECT_EQ(target.get(), ends[1]);
    EXPECT_FALSE(is_open(ends[0]));
}

TEST(Fd, ResetClosesAndCanBeRepeated) {
    const std::array<int, 2> ends = make_pipe();
    ASSERT_GE(ends[0], 0);
    Fd reader(ends[0]);
    const Fd writer(ends[1]);

    reader.reset();
    EXPECT_FALSE(reader.valid());
    EXPECT_FALSE(is_open(ends[0]));
    reader.reset();
    EXPECT_FALSE(reader.valid());
}

}  // namespace
