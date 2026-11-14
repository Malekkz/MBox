// Fuzz target for the names found in the directories of the tag database.
// They are input from outside the process: any member of the group `scada`
// can create an entry in a generation directory. The name parsers must
// accept exactly the names this library writes, whatever the bytes.

#include "region_file.h"

#include "scada/common/panic.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

// A region name a temporary name is tested against.
constexpr std::string_view REGION = "proc.io";

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    // libFuzzer hands over bytes; a directory entry is the same bytes as
    // text.
    std::string name;
    name.reserve(input.size());
    for (const std::uint8_t byte : input) {
        name.push_back(static_cast<char>(byte));
    }

    // A name that parses is exactly the name of that generation.
    const std::optional<std::uint64_t> generation =
        scada::tag::parse_generation_dir_name(name);
    if (generation.has_value() &&
        scada::tag::generation_dir_name(*generation) != name) {
        scada::panic("a generation name was accepted that is not canonical");
    }

    // A temporary name of the region is the prefix, the region, a dot and
    // digits, and nothing else.
    if (scada::tag::is_temporary_name_of(name, REGION)) {
        std::string expected(scada::tag::TEMPORARY_PREFIX);
        expected += REGION;
        expected += '.';
        const bool shaped =
            name.starts_with(expected) && name.size() > expected.size() &&
            name.find_first_not_of("0123456789", expected.size()) ==
                std::string::npos;
        if (!shaped) {
            scada::panic("a temporary name was accepted that is not one");
        }
    }
    return 0;
}
