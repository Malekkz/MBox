// Fuzz target for the parser of the temporary tag-set file, which is input
// from outside the process. A document that parses is also validated,
// hashed and built into a header image, so that the builder sees every tag
// set the parser can produce.

#include "scada/tag/header_builder.h"
#include "scada/tag/tag_set_json.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

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
    std::string detail;
    const auto tag_set = scada::tag::parse_tag_set_json(text, detail);
    if (!tag_set.ok()) {
        // Rejecting input is correct behaviour.
        return 0;
    }
    // The result is not needed: an invalid tag set is rejected here.
    static_cast<void>(scada::tag::build_header(tag_set.value(), {}, detail));
    return 0;
}
