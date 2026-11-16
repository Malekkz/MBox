#ifndef SCADA_TAG_TESTS_ATTACH_FIXTURE_H
#define SCADA_TAG_TESTS_ATTACH_FIXTURE_H

// A tag database with a running generation for the tests of attach: the
// side of scada-tagd is a GenerationHost in the test process, and the
// processes that attach are Attachments in the same process.

#include "layout.h"
#include "region_file.h"
#include "shared_memory.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/mapped_region.h"
#include "scada/common/result.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/header_builder.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace scada::tag::testing {

// Memory for the image of a region, aligned as a mapping is.
class AlignedImage {
public:
    explicit AlignedImage(std::span<const std::byte> image)
        : blocks_((image.size() + sizeof(Block) - 1) / sizeof(Block)),
          size_(image.size()) {
        std::ranges::copy(image, bytes().begin());
    }

    // The bytes of the image.
    [[nodiscard]] std::span<std::byte> bytes() {
        return std::as_writable_bytes(std::span<Block>(blocks_)).first(size_);
    }

private:
    struct alignas(REGION_ALIGNMENT) Block {
        std::array<std::byte, REGION_ALIGNMENT> bytes{};
    };
    std::vector<Block> blocks_;
    std::size_t size_;
};

// Process indexes, owner indexes and queue ids of `AttachFixture::tag_set`,
// in name order.
inline constexpr std::uint16_t PROCESS_GUEST = 0;
inline constexpr std::uint16_t PROCESS_IO = 1;
inline constexpr std::uint16_t PROCESS_TAGD = 2;
inline constexpr std::uint16_t PROCESS_WEB = 3;
inline constexpr std::size_t OWNER_IO = 0;
inline constexpr std::size_t QUEUE_CMD = 0;
inline constexpr std::size_t QUEUE_EVENTS = 1;
// Slots of the owner `io`, in name order, and its string slots.
inline constexpr std::uint32_t SLOT_LABEL = 0;
inline constexpr std::uint32_t SLOT_LEVEL = 1;
inline constexpr std::uint32_t SLOT_NOTE = 2;
inline constexpr std::uint32_t SLOT_RUNNING = 3;
inline constexpr std::size_t IO_TAG_COUNT = 4;
inline constexpr std::size_t STRING_SLOT_LABEL = 0;
inline constexpr std::size_t STRING_SLOT_NOTE = 1;
inline constexpr std::uint32_t TAGD_TIMEOUT_MS = 3000;

class AttachFixture : public TempDatabase {
protected:
    // scada-tagd; `io` with one owner, four tags of which two are strings,
    // an event queue and the results of the commands of `web`; `web`
    // without tags, which posts commands; and `guest`, which the header
    // gives to another user.
    [[nodiscard]] static TagSet tag_set() {
        TagSet set;
        set.processes = {
            {.name = "tagd",
             .uid = uid(),
             .heartbeat_timeout_ms = TAGD_TIMEOUT_MS},
            {.name = "io", .uid = uid()},
            {.name = "web", .uid = uid()},
            {.name = "guest", .uid = other_uid()},
        };
        set.owners = {{.name = "io",
                       .process = "io",
                       .change_ring_capacity = MIN_CHANGE_RING_CAPACITY}};
        set.queues = {
            {.name = "events.io", .producer = "io", .consumer = "web"},
            {.name = "cmd.io.web", .producer = "web", .consumer = "io"},
            {.name = "result.io.web", .producer = "io", .consumer = "web"}};
        set.tags = {
            {.name = "plant.level", .owner = "io", .type = TagType::FLOAT64},
            {.name = "plant.label", .owner = "io", .type = TagType::STRING},
            {.name = "plant.running", .owner = "io", .type = TagType::BOOL},
            {.name = "plant.note", .owner = "io", .type = TagType::STRING}};
        return set;
    }

    // Starts the side of scada-tagd; generation 1 on a new database. `ops`:
    // the file system calls of the retain writer, null for the system's.
    void start_host(const TagSet& set = tag_set(),
                    RetainFileOps* ops = nullptr) {
        std::string detail;
        Result<GenerationHost> host = GenerationHost::start(
            set,
            {.base_dir = base().string(),
             .tagd_uid = uid(),
             .retain_dir = host_retain_dir_,
             .retain_interval_ms = DEFAULT_RETAIN_INTERVAL_MS,
             .retain_file_ops = ops},
            detail);
        ASSERT_TRUE(host.ok()) << host.status().message() << ": " << detail;
        host_ = std::make_unique<GenerationHost>(std::move(host).value());
    }

    // Stops the side of scada-tagd as a crash would: without a word.
    void drop_host() { host_.reset(); }

    [[nodiscard]] GenerationHost& host() { return *host_; }

    [[nodiscard]] std::filesystem::path generation_dir(
        std::uint64_t generation = 1) const {
        return base() / generation_dir_name(generation);
    }

    // The file `name` of generation 1, mapped read-write, as its owner or
    // scada-tagd has it. Tests use it to damage a region.
    [[nodiscard]] MappedRegion map_file(const std::string& name) const {
        const std::filesystem::path path = generation_dir() / name;
        const Fd file = open_file(path);
        Result<MappedRegion> memory =
            MappedRegion::map(file.get(), std::filesystem::file_size(path),
                              MapAccess::READ_WRITE);
        EXPECT_TRUE(memory.ok()) << path;
        return memory.ok() ? std::move(memory).value() : MappedRegion();
    }

    // Sets the state of generation 1, as scada-tagd does.
    void set_generation_state(GenerationState state) const {
        const MappedRegion header = map_file("header");
        store_shared(
            shared_at<GlobalHeader>(header.bytes(), sizeof(RegionHeader)).state,
            to_raw(state), std::memory_order_release);
    }

    // Sets the retain directory `start_host` gives the host; empty: none.
    void use_retain_dir(std::string path) {
        host_retain_dir_ = std::move(path);
    }

private:
    std::string host_retain_dir_;
    std::unique_ptr<GenerationHost> host_;
};

}  // namespace scada::tag::testing

#endif  // SCADA_TAG_TESTS_ATTACH_FIXTURE_H
