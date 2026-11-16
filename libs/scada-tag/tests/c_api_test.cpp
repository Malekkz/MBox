// Unit tests for the C interface (contract, section 13): arguments, return
// codes, errno and the status structure. What attach does to the regions is
// tested in attachment_test.cpp.

#include "scada/tag/scada_tag.h"

#include "attach_fixture.h"
#include "layout.h"
#include "shared_memory.h"

#include "scada/common/clock.h"
#include "scada/common/mapped_region.h"
#include "scada/tag/generation_host.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using scada::MappedRegion;
using scada::tag::GenerationState;
using scada::tag::testing::PROCESS_IO;
using scada::tag::testing::QUEUE_CMD;

constexpr std::uint32_t OTHER_LAYOUT_VERSION = 2;
// The bit patterns of two FLOAT64 values, 42.5 and -7.25.
constexpr std::uint64_t LEVEL_BITS = 0x4045'4000'0000'0000U;
constexpr std::uint64_t OTHER_LEVEL_BITS = 0xC01D'0000'0000'0000U;
// A value beyond every uid.
constexpr std::int64_t NO_UID = 0x1'0000'0000;
// A wait that should end long before this has failed.
constexpr int WAIT_LIMIT_MS = 10'000;
constexpr std::int64_t NANOSECONDS_PER_MILLISECOND = 1'000'000;

// A context, detached when the test ends.
class Context {
public:
    Context() = default;
    ~Context() { scada_tag_detach(ctx_); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] scada_ctx** out() { return &ctx_; }
    [[nodiscard]] scada_ctx* get() const { return ctx_; }
    void detach() {
        scada_tag_detach(ctx_);
        ctx_ = nullptr;
    }

private:
    scada_ctx* ctx_ = nullptr;
};

class CApi : public scada::tag::testing::AttachFixture {
protected:
    void SetUp() override {
        AttachFixture::SetUp();
        base_dir_ = base().string();
    }

    [[nodiscard]] scada_attach_opts opts() const {
        return {.base_dir = base_dir_.c_str(),
                .retain_dir = nullptr,
                .tagd_uid = std::int64_t{uid()}};
    }

    [[nodiscard]] int attach(const char* process, Context& context) const {
        const scada_attach_opts options = opts();
        return scada_tag_attach_opts(process, &options, context.out());
    }

private:
    std::string base_dir_;
};

TEST_F(CApi, AttachHeartbeatStatusDetach) {
    start_host();
    Context feeder;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    ASSERT_NE(feeder.get(), nullptr);
    EXPECT_EQ(scada_tag_heartbeat(feeder.get()), SCADA_OK);

    host().heartbeat(scada::monotonic_ns());
    scada_status status{};
    EXPECT_EQ(scada_tag_status(feeder.get(), &status), SCADA_OK);
    EXPECT_EQ(status.generation, 1U);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RUNNING);
    EXPECT_EQ(status.process_index, PROCESS_IO);
    EXPECT_EQ(status.tagd_down, 0);
    EXPECT_EQ(status.regions_created, 6U);
    EXPECT_EQ(status.regions_reused, 0U);
    EXPECT_EQ(status.records_repaired, 0U);
    EXPECT_LE(status.clock_syscall, 1);
    // The fixture has one owner and four processes (contract 0.9: the two
    // counts took the place of `reserved1`).
    EXPECT_EQ(status.owner_count, 1U);
    EXPECT_EQ(status.process_count, 4U);

    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), false);
    feeder.detach();
    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), true);

    // The regions stayed: the next attach reuses them.
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    EXPECT_EQ(scada_tag_status(feeder.get(), &status), SCADA_OK);
    EXPECT_EQ(status.regions_reused, 6U);
}

TEST_F(CApi, TheStatusStructureHasItsDocumentedSize) {
    EXPECT_EQ(sizeof(scada_status), 32U);
    EXPECT_EQ(sizeof(scada_queue_stats_t), 80U);
    EXPECT_EQ(sizeof(scada_event_record), 256U);
    EXPECT_EQ(sizeof(scada_write_request), 64U);
    EXPECT_EQ(sizeof(scada_write_result), 32U);
    EXPECT_EQ(SCADA_E_BUSY, 14);
    EXPECT_EQ(SCADA_E_IO, 13);
    EXPECT_EQ(SCADA_E_PERMISSION, 12);
}

TEST_F(CApi, NullArgumentsAreInvalid) {
    start_host();
    Context feeder;
    const scada_attach_opts options = opts();
    EXPECT_EQ(scada_tag_attach_opts("io", &options, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_attach_opts(nullptr, &options, feeder.out()),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_attach(nullptr, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_heartbeat(nullptr), SCADA_E_INVALID);
    scada_status status{};
    EXPECT_EQ(scada_tag_status(nullptr, &status), SCADA_E_INVALID);
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    EXPECT_EQ(scada_tag_status(feeder.get(), nullptr), SCADA_E_INVALID);
    scada_tag_detach(nullptr);  // does nothing
}

TEST_F(CApi, AFailedAttachLeavesNoContext) {
    start_host();
    Context feeder;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    // A pointer that holds something before the call holds nothing after.
    scada_ctx* context = feeder.get();
    const scada_attach_opts options = opts();
    EXPECT_EQ(scada_tag_attach_opts("stranger", &options, &context),
              SCADA_E_NOT_FOUND);
    EXPECT_EQ(context, nullptr);
}

TEST_F(CApi, WhoMayNotAttachAndWhy) {
    start_host();
    Context none;
    // Not in the process table; the wrong user; no process name.
    EXPECT_EQ(attach("stranger", none), SCADA_E_NOT_FOUND);
    EXPECT_EQ(attach("guest", none), SCADA_E_PERMISSION);
    EXPECT_EQ(attach("No Name", none), SCADA_E_INVALID);
    EXPECT_EQ(none.get(), nullptr);

    Context feeder;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    Context second;
    EXPECT_EQ(attach("io", second), SCADA_E_BUSY);
    EXPECT_EQ(second.get(), nullptr);
}

TEST_F(CApi, TheUidOfTagdIsCheckedOrLookedUp) {
    start_host();
    Context none;
    scada_attach_opts options = opts();
    options.tagd_uid = std::int64_t{other_uid()};
    EXPECT_EQ(scada_tag_attach_opts("io", &options, none.out()),
              SCADA_E_PERMISSION);
    options.tagd_uid = -2;
    EXPECT_EQ(scada_tag_attach_opts("io", &options, none.out()),
              SCADA_E_INVALID);
    options.tagd_uid = NO_UID;
    EXPECT_EQ(scada_tag_attach_opts("io", &options, none.out()),
              SCADA_E_INVALID);
    // -1: the user scada-tagd. Either there is none on this machine, or it
    // is not the user of the test that owns the base directory.
    options.tagd_uid = -1;
    EXPECT_EQ(scada_tag_attach_opts("io", &options, none.out()),
              SCADA_E_PERMISSION);
}

TEST_F(CApi, WithoutOptionsTheDefaultDatabaseIsUsed) {
    // /dev/shm/scada of this machine, if any, is not the database of the
    // test and does not list the process.
    Context none;
    EXPECT_NE(scada_tag_attach("no-such-process-of-the-test", none.out()),
              SCADA_OK);
    EXPECT_EQ(none.get(), nullptr);
}

TEST_F(CApi, OperatingSystemErrorsComeWithErrno) {
    Context none;
    // No generation yet.
    errno = 0;
    EXPECT_EQ(attach("io", none), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);

    const std::string nowhere = (root() / "nowhere").string();
    scada_attach_opts options = opts();
    options.base_dir = nowhere.c_str();
    errno = 0;
    EXPECT_EQ(scada_tag_attach_opts("io", &options, none.out()), SCADA_E_IO);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(CApi, Conformance12AnotherLayoutVersionRefusesToAttach) {
    start_host();
    {
        const MappedRegion header = map_file("header");
        scada::tag::shared_at<scada::tag::RegionHeader>(header.bytes(), 0)
            .layout_version = OTHER_LAYOUT_VERSION;
    }
    Context none;
    EXPECT_EQ(attach("io", none), SCADA_E_LAYOUT);
    EXPECT_EQ(none.get(), nullptr);
}

TEST_F(CApi, ARetiringGenerationIsReportedEverywhere) {
    start_host();
    Context feeder;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    set_generation_state(GenerationState::RETIRING);
    EXPECT_EQ(scada_tag_heartbeat(feeder.get()), SCADA_E_RETIRING);
    scada_status status{};
    EXPECT_EQ(scada_tag_status(feeder.get(), &status), SCADA_E_RETIRING);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RETIRING);
    EXPECT_EQ(status.generation, 1U);
    Context web;
    EXPECT_EQ(attach("web", web), SCADA_E_RETIRING);
}

TEST_F(CApi, TagdDownIsReportedAndTheContextKeepsWorking) {
    start_host();
    Context feeder;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    host().stop();
    scada_status status{};
    EXPECT_EQ(scada_tag_status(feeder.get(), &status), SCADA_E_TAGD_DOWN);
    EXPECT_EQ(status.tagd_down, 1);
    EXPECT_EQ(status.generation_state, SCADA_GENERATION_RUNNING);
    EXPECT_EQ(scada_tag_heartbeat(feeder.get()), SCADA_OK);
}

TEST_F(CApi, TheTagStructuresHaveTheirDocumentedSizes) {
    EXPECT_EQ(sizeof(scada_value), 48U);
    EXPECT_EQ(sizeof(scada_tag_info_t), 168U);
    EXPECT_EQ(SCADA_INVALID_HANDLE, 0xFFFF'FFFFU);
    EXPECT_EQ(SCADA_E_NOT_OWNER, 4);
    EXPECT_EQ(SCADA_E_TYPE, 5);
    EXPECT_EQ(SCADA_FLAG_OWNER_DOWN, 1 << 5);
    EXPECT_EQ(SCADA_FLAG_LOCAL_TIME, 1 << 11);
}

TEST_F(CApi, ATagIsResolvedWrittenAndReadThroughTheCInterface) {
    start_host();
    Context feeder;
    Context web;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    ASSERT_EQ(attach("web", web), SCADA_OK);

    std::uint32_t own = 0;
    std::uint32_t level = 0;
    ASSERT_EQ(scada_tag_resolve(feeder.get(), "plant.level", &own), SCADA_OK);
    ASSERT_EQ(scada_tag_resolve(web.get(), "plant.level", &level), SCADA_OK);
    EXPECT_EQ(own, level);

    scada_tag_info_t info{};
    ASSERT_EQ(scada_tag_info(web.get(), level, &info), SCADA_OK);
    EXPECT_EQ(std::string_view(std::data(info.name)), "plant.level");
    EXPECT_EQ(info.type, SCADA_TYPE_FLOAT64);

    scada_value written{};
    written.type = SCADA_TYPE_FLOAT64;
    written.quality = SCADA_QUALITY_GOOD;
    written.value = LEVEL_BITS;
    ASSERT_EQ(scada_tag_write(feeder.get(), own, &written), SCADA_OK);
    EXPECT_EQ(scada_tag_write(web.get(), level, &written), SCADA_E_NOT_OWNER);

    scada_value value{};
    ASSERT_EQ(scada_tag_read(web.get(), level, &value), SCADA_OK);
    EXPECT_EQ(value.value, LEVEL_BITS);
    EXPECT_EQ(value.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(value.quality_flags, SCADA_FLAG_LOCAL_TIME);

    written.value = OTHER_LEVEL_BITS;
    ASSERT_EQ(scada_tag_write_many(feeder.get(), &own, 1, &written), SCADA_OK);
    int status = -1;
    ASSERT_EQ(scada_tag_read_many(web.get(), &level, 1, &value, &status),
              SCADA_OK);
    EXPECT_EQ(status, SCADA_OK);
    EXPECT_EQ(value.value, OTHER_LEVEL_BITS);
    ASSERT_EQ(scada_tag_read_many(web.get(), &level, 1, &value, nullptr),
              SCADA_OK);

    std::uint32_t note = 0;
    ASSERT_EQ(scada_tag_resolve(feeder.get(), "plant.note", &note), SCADA_OK);
    const std::string_view text = "valve 3 open";
    ASSERT_EQ(scada_tag_write_string(feeder.get(), note, text.data(),
                                     text.size(), SCADA_QUALITY_GOOD, 0, 0),
              SCADA_OK);
    ASSERT_EQ(scada_tag_resolve(web.get(), "plant.note", &note), SCADA_OK);
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = 0;
    scada_value meta{};
    ASSERT_EQ(scada_tag_read_string(web.get(), note, buffer.data(),
                                    buffer.size(), &length, &meta),
              SCADA_OK);
    EXPECT_EQ(std::string_view(buffer.data(), length), text);
    EXPECT_EQ(meta.type, SCADA_TYPE_STRING);
    // NULL with capacity 0 asks for the length alone (issue #42, point 5).
    std::size_t length_only = 0;
    scada_value meta_only{};
    ASSERT_EQ(scada_tag_read_string(web.get(), note, nullptr, 0, &length_only,
                                    &meta_only),
              SCADA_OK);
    EXPECT_EQ(length_only, text.size());
    EXPECT_EQ(meta_only.quality, SCADA_QUALITY_GOOD);
    EXPECT_EQ(meta_only.update_mono_ns, meta.update_mono_ns);
    // A buffer that is not NULL with capacity 0 is still too small.
    EXPECT_EQ(scada_tag_read_string(web.get(), note, buffer.data(), 0,
                                    &length_only, nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(length_only, text.size());
    // An empty text needs no pointer.
    EXPECT_EQ(scada_tag_write_string(feeder.get(), note, nullptr, 0,
                                     SCADA_QUALITY_GOOD, 0, 0),
              SCADA_OK);

    // The owner detaches: its tags read as down at once.
    feeder.detach();
    ASSERT_EQ(scada_tag_read(web.get(), level, &value), SCADA_OK);
    EXPECT_EQ(value.quality, SCADA_QUALITY_BAD);
    EXPECT_NE(value.quality_flags & SCADA_FLAG_OWNER_DOWN, 0);
}

TEST_F(CApi, NullArgumentsOfTheTagFunctionsAreInvalid) {
    start_host();
    Context feeder;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    scada_ctx* const ctx = feeder.get();
    std::uint32_t handle = 0;
    ASSERT_EQ(scada_tag_resolve(ctx, "plant.level", &handle), SCADA_OK);
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    scada_tag_info_t info{};
    std::array<char, SCADA_STRING_MAX> buffer{};
    std::size_t length = 0;

    std::uint32_t none = 0;
    EXPECT_EQ(scada_tag_resolve(nullptr, "plant.level", &none),
              SCADA_E_INVALID);
    EXPECT_EQ(none, SCADA_INVALID_HANDLE);
    EXPECT_EQ(scada_tag_resolve(ctx, nullptr, &none), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_resolve(ctx, "plant.level", nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_resolve(ctx, "plant.none", &none), SCADA_E_NOT_FOUND);
    EXPECT_EQ(none, SCADA_INVALID_HANDLE);

    EXPECT_EQ(scada_tag_info(nullptr, handle, &info), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_info(ctx, handle, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read(nullptr, handle, &value), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read(ctx, handle, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write(nullptr, handle, &value), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write(ctx, handle, nullptr), SCADA_E_INVALID);

    EXPECT_EQ(scada_tag_read_many(nullptr, &handle, 1, &value, nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read_many(ctx, nullptr, 1, &value, nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read_many(ctx, &handle, 1, nullptr, nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read_many(ctx, nullptr, 0, nullptr, nullptr), SCADA_OK);
    EXPECT_EQ(scada_tag_write_many(nullptr, &handle, 1, &value),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_many(ctx, nullptr, 1, &value), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_many(ctx, &handle, 1, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_many(ctx, nullptr, 0, nullptr), SCADA_OK);

    std::uint32_t note = 0;
    ASSERT_EQ(scada_tag_resolve(ctx, "plant.note", &note), SCADA_OK);
    EXPECT_EQ(scada_tag_read_string(nullptr, note, buffer.data(), buffer.size(),
                                    &length, nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read_string(ctx, note, nullptr, buffer.size(), &length,
                                    nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_read_string(ctx, note, buffer.data(), buffer.size(),
                                    nullptr, nullptr),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_string(nullptr, note, "x", 1, 0, 0, 0),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_string(ctx, note, nullptr, 1, 0, 0, 0),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_tag_write_string(ctx, note, buffer.data(),
                                     SCADA_STRING_MAX + 1, 0, 0, 0),
              SCADA_E_INVALID);
}

TEST_F(CApi, TheChangeStructuresHaveTheirDocumentedSizes) {
    EXPECT_EQ(sizeof(scada_change), 32U);
    EXPECT_EQ(sizeof(scada_sub_stats_t), 48U);
    EXPECT_EQ(SCADA_WAIT_SLICE_MS, 100);
    EXPECT_EQ(offsetof(scada_sub_stats_t, last_gap), 45U);
    EXPECT_EQ(SCADA_GAP_NONE, 0);
    EXPECT_EQ(SCADA_GAP_OWNER_BACK, 5);
}

// A change written through the C interface arrives through it, and the
// wait set and notify work there too.
TEST_F(CApi, AChangeIsWrittenAndReceivedThroughTheCInterface) {
    start_host();
    Context feeder;
    Context web;
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    ASSERT_EQ(attach("web", web), SCADA_OK);
    scada_sub* sub = nullptr;
    ASSERT_EQ(scada_sub_open(web.get(), "io", &sub), SCADA_OK);
    scada_waitset* set = nullptr;
    ASSERT_EQ(scada_waitset_create(web.get(), &set), SCADA_OK);
    ASSERT_EQ(scada_waitset_add_sub(set, sub), SCADA_OK);
    ASSERT_EQ(scada_waitset_add_cycle(set, PROCESS_IO), SCADA_OK);

    std::uint32_t handle = 0;
    ASSERT_EQ(scada_tag_resolve(feeder.get(), "plant.level", &handle),
              SCADA_OK);
    scada_value value{};
    value.type = SCADA_TYPE_FLOAT64;
    value.value = LEVEL_BITS;
    ASSERT_EQ(scada_tag_write(feeder.get(), handle, &value), SCADA_OK);
    ASSERT_EQ(scada_tag_notify(feeder.get()), SCADA_OK);
    EXPECT_EQ(scada_waitset_wait(set, 0), SCADA_OK);
    scada_change change{};
    ASSERT_EQ(scada_sub_next(sub, &change, 0), SCADA_OK);
    EXPECT_EQ(change.handle, handle);
    EXPECT_EQ(change.value, LEVEL_BITS);
    EXPECT_EQ(scada_sub_next(sub, &change, 0), SCADA_E_TIMEOUT);
    scada_sub_stats_t stats{};
    ASSERT_EQ(scada_sub_stats(sub, &stats), SCADA_OK);
    EXPECT_EQ(stats.received, 1U);
    EXPECT_EQ(stats.owner_index, scada::tag::testing::OWNER_IO);
    EXPECT_EQ(stats.process_index, PROCESS_IO);
    int descriptor = -1;
    ASSERT_EQ(scada_waitset_fd(set, &descriptor), SCADA_OK);
    EXPECT_GE(descriptor, 0);
    scada_waitset_destroy(set);
    scada_sub_close(sub);
}

TEST_F(CApi, NullArgumentsOfTheChangeFunctionsAreInvalid) {
    start_host();
    Context web;
    ASSERT_EQ(attach("web", web), SCADA_OK);
    scada_ctx* const ctx = web.get();
    EXPECT_EQ(scada_tag_notify(nullptr), SCADA_E_INVALID);

    scada_sub* sub = nullptr;
    EXPECT_EQ(scada_sub_open(nullptr, "io", &sub), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_open(ctx, nullptr, &sub), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_open(ctx, "io", nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_open(ctx, "nobody", &sub), SCADA_E_NOT_FOUND);
    EXPECT_EQ(sub, nullptr);
    ASSERT_EQ(scada_sub_open(ctx, "io", &sub), SCADA_OK);
    scada_change change{};
    scada_sub_stats_t stats{};
    EXPECT_EQ(scada_sub_next(nullptr, &change, 0), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_next(sub, nullptr, 0), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_next(sub, &change, -1), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_stats(nullptr, &stats), SCADA_E_INVALID);
    EXPECT_EQ(scada_sub_stats(sub, nullptr), SCADA_E_INVALID);

    scada_waitset* set = nullptr;
    int descriptor = -1;
    EXPECT_EQ(scada_waitset_create(nullptr, &set), SCADA_E_INVALID);
    EXPECT_EQ(set, nullptr);
    EXPECT_EQ(scada_waitset_create(ctx, nullptr), SCADA_E_INVALID);
    ASSERT_EQ(scada_waitset_create(ctx, &set), SCADA_OK);
    EXPECT_EQ(scada_waitset_add_sub(nullptr, sub), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_add_sub(set, nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_add_cycle(nullptr, 0), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_wait(nullptr, 0), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_wait(set, -1), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_fd(nullptr, &descriptor), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_fd(set, nullptr), SCADA_E_INVALID);

    // Closing and destroying NULL does nothing.
    scada_sub_close(nullptr);
    scada_waitset_destroy(nullptr);
    scada_waitset_destroy(set);
    scada_sub_close(sub);
}

TEST_F(CApi, AnEventIsPostedAndTakenThroughTheCInterface) {
    start_host();
    Context web;
    Context feeder;
    // The consumer first, so that the producer finds its region at once.
    ASSERT_EQ(attach("web", web), SCADA_OK);
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    scada_queue* out = nullptr;
    scada_queue* reader = nullptr;
    ASSERT_EQ(scada_queue_open(feeder.get(), "events.io", &out), SCADA_OK);
    ASSERT_EQ(scada_queue_open(web.get(), "events.io", &reader), SCADA_OK);

    scada_event_record record{};
    record.kind = SCADA_EVENT_ALARM;
    record.payload_len = 1;
    record.payload[0] = 1;
    record.source_handle = SCADA_INVALID_HANDLE;
    ASSERT_EQ(scada_queue_post(out, &record, sizeof(record)), SCADA_OK);
    ASSERT_EQ(scada_tag_notify(feeder.get()), SCADA_OK);
    scada_event_record got{};
    // The queue region of `io` did not exist when `web` attached: the wait
    // looks for it again (at most once a second).
    ASSERT_EQ(scada_queue_next(reader, &got, sizeof(got), WAIT_LIMIT_MS),
              SCADA_OK);
    EXPECT_EQ(got.seq, 0U);
    EXPECT_EQ(got.kind, SCADA_EVENT_ALARM);
    EXPECT_EQ(scada_queue_ack(reader), SCADA_OK);

    scada_waitset* set = nullptr;
    ASSERT_EQ(scada_waitset_create(web.get(), &set), SCADA_OK);
    ASSERT_EQ(scada_waitset_add_queue(set, reader), SCADA_OK);
    EXPECT_EQ(scada_waitset_wait(set, 0), SCADA_E_TIMEOUT);
    ASSERT_EQ(scada_queue_post(out, &record, sizeof(record)), SCADA_OK);
    EXPECT_EQ(scada_waitset_wait(set, 0), SCADA_OK);
    ASSERT_EQ(scada_queue_next(reader, &got, sizeof(got), 0), SCADA_OK);
    EXPECT_EQ(got.seq, 1U);
    scada_waitset_destroy(set);
    scada_queue_close(reader);
    scada_queue_close(out);
}

TEST_F(CApi, ARequestIsPostedThroughTheCInterface) {
    start_host();
    Context web;
    Context driver;
    ASSERT_EQ(attach("io", driver), SCADA_OK);
    ASSERT_EQ(attach("web", web), SCADA_OK);
    scada_queue* out = nullptr;
    scada_queue* reader = nullptr;
    ASSERT_EQ(scada_queue_open(web.get(), "cmd.io.web", &out), SCADA_OK);
    ASSERT_EQ(scada_queue_open(driver.get(), "cmd.io.web", &reader), SCADA_OK);
    scada_write_request request{};
    request.target_handle = 1;
    request.operation = SCADA_OP_READ_NOW;
    request.source = SCADA_SOURCE_AUTO;
    request.expiry_mono_ns =
        scada::monotonic_ns() + (std::int64_t{SCADA_REQUEST_LIFETIME_MAX_MS} *
                                 NANOSECONDS_PER_MILLISECOND);
    ASSERT_EQ(scada_queue_post(out, &request, sizeof(request)), SCADA_OK);
    scada_write_request got{};
    ASSERT_EQ(scada_queue_next(reader, &got, sizeof(got), WAIT_LIMIT_MS),
              SCADA_OK);
    EXPECT_EQ(got.request_id, std::uint64_t{QUEUE_CMD} << 48U);
    EXPECT_EQ(got.operation, SCADA_OP_READ_NOW);
    EXPECT_EQ(scada_queue_ack(reader), SCADA_E_INVALID);
    scada_queue_close(reader);
    scada_queue_close(out);
}

TEST_F(CApi, NullArgumentsOfTheQueueFunctionsAreInvalid) {
    start_host();
    Context web;
    ASSERT_EQ(attach("web", web), SCADA_OK);
    scada_ctx* const ctx = web.get();
    scada_queue* queue = nullptr;
    EXPECT_EQ(scada_queue_open(nullptr, "events.io", &queue), SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_open(ctx, nullptr, &queue), SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_open(ctx, "events.io", nullptr), SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_open(ctx, "events.none", &queue), SCADA_E_NOT_FOUND);
    EXPECT_EQ(queue, nullptr);
    ASSERT_EQ(scada_queue_open(ctx, "events.io", &queue), SCADA_OK);
    scada_event_record record{};
    EXPECT_EQ(scada_queue_post(nullptr, &record, sizeof(record)),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_post(queue, nullptr, sizeof(record)),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_next(nullptr, &record, sizeof(record), 0),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_next(queue, nullptr, sizeof(record), 0),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_next(queue, &record, sizeof(record), -1),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_next(queue, &record, sizeof(record) - 1, 0),
              SCADA_E_INVALID);
    EXPECT_EQ(scada_queue_ack(nullptr), SCADA_E_INVALID);
    scada_waitset* set = nullptr;
    ASSERT_EQ(scada_waitset_create(ctx, &set), SCADA_OK);
    EXPECT_EQ(scada_waitset_add_queue(nullptr, queue), SCADA_E_INVALID);
    EXPECT_EQ(scada_waitset_add_queue(set, nullptr), SCADA_E_INVALID);
    scada_waitset_destroy(set);
    scada_queue_close(nullptr);
    scada_queue_close(queue);
}

}  // namespace
