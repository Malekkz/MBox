#include "scada/common/panic.h"

#include "scada/common/static_string.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string_view>

#include <sys/types.h>
#include <unistd.h>

namespace scada {

void panic(StaticString message) noexcept {
    constexpr std::string_view PREFIX = "scada: fatal: ";
    // One buffer and one write, so that the line is not interleaved with
    // output of other threads. A longer message is cut.
    std::array<char, 256> line{};
    std::size_t size = 0;
    const auto append = [&line, &size](std::string_view text) {
        const std::size_t count = std::min(text.size(), line.size() - size);
        std::copy_n(text.begin(), count, line.begin() + size);
        size += count;
    };
    append(PREFIX);
    append(message.view().substr(0, line.size() - PREFIX.size() - 1));
    append("\n");

    // Nothing can be done if the write fails; the process aborts either way.
    const ssize_t ignored = ::write(STDERR_FILENO, line.data(), size);
    static_cast<void>(ignored);
    std::abort();
}

}  // namespace scada
