// Fuzz target for the parser of the JOURNAL_STREAM environment variable
// ("<device>:<inode>" in decimal), which is input from outside the process.

#include "log_internal.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <unistd.h>

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    // libFuzzer hands over bytes; the parser takes the same bytes as text.
    std::string text;
    text.reserve(input.size());
    for (const std::uint8_t byte : input) {
        text.push_back(static_cast<char>(byte));
    }
    // The results are not needed: rejecting input is correct behaviour.
    static_cast<void>(scada::log_internal::parse_decimal(text));
    static_cast<void>(
        scada::log_internal::is_journal_stream(text, STDERR_FILENO));
    return 0;
}
