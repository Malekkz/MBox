#include "check.h"
#include "scada/common/fd.h"

#include <array>
#include <cerrno>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

using scada::Fd;

static_assert(!std::is_copy_constructible_v<Fd>);
static_assert(!std::is_copy_assignable_v<Fd>);
static_assert(std::is_nothrow_move_constructible_v<Fd>);
static_assert(std::is_nothrow_move_assignable_v<Fd>);

bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

// Returns both ends of a new pipe.
std::array<int, 2> make_pipe() {
    std::array<int, 2> ends{-1, -1};
    if (::pipe(ends.data()) != 0) {
        ends = {-1, -1};
    }
    return ends;
}

}  // namespace

int main() {
    scada::test::Checker checker;

    {
        const Fd empty;
        checker.expect(!empty.valid() && empty.get() == -1, "default: invalid");
        const Fd failed(-1);
        checker.expect(!failed.valid() && failed.get() == -1,
                       "negative descriptor: invalid");
    }
    {
        const std::array<int, 2> ends = make_pipe();
        checker.expect(ends[0] >= 0, "pipe created");
        {
            const Fd reader(ends[0]);
            const Fd writer(ends[1]);
            checker.expect(reader.valid() && reader.get() == ends[0],
                           "owning: valid");
            checker.expect(is_open(ends[0]), "owning: open while alive");
        }
        checker.expect(!is_open(ends[0]) && !is_open(ends[1]),
                       "destructor closes");
    }
    {
        const std::array<int, 2> ends = make_pipe();
        Fd first(ends[0]);
        Fd second(ends[1]);

        Fd moved(std::move(first));
        checker.expect(moved.get() == ends[0], "move construction transfers");
        // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is tested
        checker.expect(!first.valid(), "move construction empties the source");

        moved = std::move(second);
        checker.expect(moved.get() == ends[1], "move assignment transfers");
        checker.expect(!is_open(ends[0]),
                       "move assignment closes the previous descriptor");

        moved.reset();
        checker.expect(!moved.valid() && !is_open(ends[1]), "reset closes");
        moved.reset();
        checker.expect(!moved.valid(), "reset twice is harmless");
    }

    return checker.exit_code();
}
