// Fuzz target for the reader of the `header` region. The region is memory
// another process wrote (coding standard, section 9): whatever its bytes,
// the reader must refuse it or return contents whose every index and offset
// can be used without leaving the region.

#include "header_view.h"
#include "layout.h"

#include "scada/common/panic.h"
#include "scada/common/result.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// The generation of the seed corpus.
constexpr std::uint64_t GENERATION = 1;

// Memory aligned as a mapping is.
struct alignas(scada::tag::REGION_ALIGNMENT) Block {
    std::array<std::byte, scada::tag::REGION_ALIGNMENT> bytes{};
};

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is set by libFuzzer
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    using scada::tag::HeaderContents;

    const std::span<const std::uint8_t> input(data, size);
    std::vector<Block> blocks((size + sizeof(Block) - 1) / sizeof(Block));
    const std::span<std::byte> region =
        std::as_writable_bytes(std::span<Block>(blocks)).first(size);
    std::ranges::transform(input, region.begin(),
                           [](std::uint8_t byte) { return std::byte{byte}; });

    std::string detail;
    const scada::Result<HeaderContents> read =
        scada::tag::read_header(region, GENERATION, detail);
    if (!read.ok()) {
        if (detail.empty()) {
            scada::panic("a header was refused without a reason");
        }
        return 0;
    }
    const HeaderContents& contents = read.value();
    if (contents.processes.empty()) {
        scada::panic("a header without a process was accepted");
    }
    // Everything attach does with the contents stays inside the region; a
    // step outside ends the process in the helpers or under the sanitizer.
    std::uint64_t tags = 0;
    for (std::size_t owner = 0; owner < contents.owners.size(); ++owner) {
        if (contents.owners[owner].process_index >= contents.processes.size()) {
            scada::panic("an owner of no process was accepted");
        }
        tags += contents.owners[owner].tag_count;
        for (std::uint32_t slot = 0; slot < contents.owners[owner].tag_count;
             ++slot) {
            static_cast<void>(
                scada::tag::slot_type(region, contents, owner, slot));
        }
    }
    // What resolve and info do: every entry of the directory, and a search
    // for the name of each entry that is valid.
    for (std::size_t index = 0; index < contents.tag_count; ++index) {
        const std::optional<scada::tag::TagEntry> entry =
            scada::tag::directory_entry(region, contents, index);
        if (!entry.has_value()) {
            continue;
        }
        if (entry->owner_index >= contents.owners.size() ||
            entry->slot >= contents.owners[entry->owner_index].tag_count) {
            scada::panic("a tag of no owner or slot was accepted");
        }
        static_cast<void>(scada::tag::find_tag(region, contents, entry->name));
    }
    static_cast<void>(scada::tag::find_tag(region, contents, "plant.level"));
    if (tags != contents.tag_count) {
        scada::panic("tag counts that do not add up were accepted");
    }
    for (const scada::tag::QueueInfo& queue : contents.queues) {
        if (queue.producer_process >= contents.processes.size() ||
            queue.consumer_process >= contents.processes.size()) {
            scada::panic("a queue of no process was accepted");
        }
    }
    return 0;
}
