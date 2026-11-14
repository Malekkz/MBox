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

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {

using scada::MappedRegion;
using scada::tag::GenerationState;
using scada::tag::testing::PROCESS_IO;

constexpr std::uint32_t OTHER_LAYOUT_VERSION = 2;
// A value beyond every uid.
constexpr std::int64_t NO_UID = 0x1'0000'0000;

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
    EXPECT_EQ(status.regions_created, 5U);
    EXPECT_EQ(status.regions_reused, 0U);
    EXPECT_EQ(status.records_repaired, 0U);
    EXPECT_EQ(status.reserved0, 0);
    EXPECT_EQ(status.reserved1, 0U);

    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), false);
    feeder.detach();
    static_cast<void>(host().check(scada::monotonic_ns()));
    EXPECT_EQ(host().is_down("io"), true);

    // The regions stayed: the next attach reuses them.
    ASSERT_EQ(attach("io", feeder), SCADA_OK);
    EXPECT_EQ(scada_tag_status(feeder.get(), &status), SCADA_OK);
    EXPECT_EQ(status.regions_reused, 5U);
}

TEST_F(CApi, TheStatusStructureHasItsDocumentedSize) {
    EXPECT_EQ(sizeof(scada_status), 32U);
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

}  // namespace
