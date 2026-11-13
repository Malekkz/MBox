// Fuzz target for the toy parser. A fuzz target hands the raw input to the
// parser and nothing else; the sanitizers decide whether the run failed.

#include "scada/toy/toy_parser.h"

#include <cstddef>
#include <cstdint>
#include <span>

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    // The result is not needed: rejecting input is correct behaviour.
    static_cast<void>(scada::toy::parse(std::as_bytes(input)));
    return 0;
}
