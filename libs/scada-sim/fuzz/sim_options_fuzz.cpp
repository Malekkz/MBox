// Fuzz target for the command line of scada-sim, which is input from outside
// the process. The input is split at NUL bytes into arguments. A `tag-set`
// command line that parses is also made into a tag set and its file, and
// the file must read back as the same tag set; a mismatch aborts, so the
// fuzzer reports it.

#include "scada/common/result.h"
#include "scada/sim/sim.h"
#include "scada/sim/tag_set.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/tag_set_json.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// More arguments than any valid command line has.
constexpr std::size_t MAX_ARGUMENTS = 256;
// Larger tag sets are only parsed, so that one input stays fast; the code
// that makes the tag set does the same for every size.
constexpr std::uint64_t MAX_FUZZED_TAGS = 4096;
constexpr std::uint32_t USER = 1000;

// Makes the tag set of `options` and checks that its file reads back with
// the same config hash.
void make_and_read_back(const scada::sim::Options& options) {
    std::string detail;
    const scada::Result<scada::tag::TagSet> tag_set = scada::sim::make_tag_set(
        scada::sim::to_tag_set_options(options, USER), detail);
    if (!tag_set.ok()) {
        // Refusing a layout is correct behaviour.
        return;
    }
    const std::string text = scada::sim::tag_set_json(tag_set.value());
    const scada::Result<scada::tag::TagSet> back =
        scada::tag::parse_tag_set_json(text, detail);
    if (!back.ok()) {
        std::abort();
    }
    const auto hash = scada::tag::config_hash(tag_set.value(), detail);
    const auto hash_back = scada::tag::config_hash(back.value(), detail);
    if (!hash.ok() || !hash_back.ok() || hash.value() != hash_back.value()) {
        std::abort();
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    std::string text;
    text.reserve(input.size());
    for (const std::uint8_t byte : input) {
        text.push_back(static_cast<char>(byte));
    }
    std::vector<std::string_view> args;
    std::string_view rest(text);
    while (!rest.empty() && args.size() < MAX_ARGUMENTS) {
        const std::size_t end = rest.find('\0');
        args.push_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view()
                                             : rest.substr(end + 1);
    }
    std::string detail;
    const scada::Result<scada::sim::Options> options =
        scada::sim::parse_options(args, detail);
    if (!options.ok()) {
        // Rejecting input is correct behaviour.
        return 0;
    }
    if (options.value().command == scada::sim::Command::TAG_SET &&
        options.value().layout.tags <= MAX_FUZZED_TAGS) {
        make_and_read_back(options.value());
    }
    return 0;
}
