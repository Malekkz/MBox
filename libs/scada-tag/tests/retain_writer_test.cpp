// Unit tests for RetainWriter, the thread of scada-tagd that writes the
// retain files (decision D-45): what is queued for an owner replaces what
// still waits for it, submitting and collecting never wait for a file
// being written, and a stop ends after the file in flight without writing
// what still waits.

#include "retain_file.h"
#include "retain_keeper.h"

#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace scada::tag;
using namespace std::chrono_literals;

// Bounds of the waits of these tests; they end much sooner when the code
// is right.
constexpr std::chrono::milliseconds LIMIT = 5000ms;
// What "returns at once" means while a file is being flushed.
constexpr std::chrono::milliseconds PROMPT = 1000ms;
// Not a descriptor: the fake below never uses it.
constexpr int NO_DIRECTORY = -1;

// File operations in memory whose file fsync blocks until released, as a
// slow storage would.
class BlockingOps final : public RetainFileOps {
public:
    int create(int /*dir_fd*/, const std::string& name,
               std::unique_ptr<RetainWriteFile>& out) override {
        {
            const std::scoped_lock lock(mutex_);
            created_.push_back(name);
        }
        out = std::make_unique<File>(*this);
        return 0;
    }
    int rename(int /*dir_fd*/, const std::string& /*from*/,
               const std::string& target) noexcept override {
        const std::scoped_lock lock(mutex_);
        renamed_.push_back(target);
        return 0;
    }
    int sync_directory(int /*dir_fd*/) noexcept override { return 0; }
    int unlink(int /*dir_fd*/, const std::string& name) noexcept override {
        const std::scoped_lock lock(mutex_);
        unlinked_.push_back(name);
        return name.starts_with(".tmp.") ? ENOENT : 0;
    }

    // Lets every fsync, from now on, return.
    void release() {
        {
            const std::scoped_lock lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    // Waits until a file fsync is blocked.
    [[nodiscard]] bool wait_syncing() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, LIMIT, [this] { return syncing_ > 0; });
    }
    [[nodiscard]] std::vector<std::string> renamed() const {
        const std::scoped_lock lock(mutex_);
        return renamed_;
    }
    [[nodiscard]] std::vector<std::string> unlinked() const {
        const std::scoped_lock lock(mutex_);
        return unlinked_;
    }

private:
    class File final : public RetainWriteFile {
    public:
        explicit File(BlockingOps& ops) : ops_(ops) {}
        int set_mode(mode_t /*mode*/) noexcept override { return 0; }
        int write(std::span<const std::byte> bytes,
                  std::size_t& written) noexcept override {
            written = bytes.size();
            return 0;
        }
        int sync() noexcept override { return ops_.block(); }

    private:
        BlockingOps& ops_;
    };

    int block() noexcept {
        std::unique_lock lock(mutex_);
        ++syncing_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        return 0;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> created_;
    std::vector<std::string> renamed_;
    std::vector<std::string> unlinked_;
    int syncing_ = 0;
    bool released_ = false;
};

RetainFile one_value(std::uint64_t value) {
    RetainFile file;
    file.values = {{"plant.mode", TagType::INT64, value, 0}};
    return file;
}

// True if submitting and collecting return within PROMPT, with nothing
// collected yet.
::testing::AssertionResult calls_return_at_once(RetainWriter& writer) {
    auto calls = std::async(std::launch::async, [&writer] {
        writer.submit("web", one_value(2), 2);
        writer.submit_removal({"gone.bin"});
        return writer.take_done().size();
    });
    if (calls.wait_for(PROMPT) != std::future_status::ready) {
        return ::testing::AssertionFailure() << "a call waited";
    }
    const std::size_t done = calls.get();
    if (done != 0) {
        return ::testing::AssertionFailure() << done << " results already";
    }
    return ::testing::AssertionSuccess();
}

TEST(RetainWriter, SubmitAndCollectNeverWaitForAFileBeingFlushed) {
    BlockingOps ops;
    RetainWriter writer(NO_DIRECTORY, ops);
    writer.submit("io", one_value(1), 1);
    ASSERT_TRUE(ops.wait_syncing());
    // What the checks of scada-tagd call while the writer is in fsync.
    EXPECT_TRUE(calls_return_at_once(writer));
    EXPECT_FALSE(writer.wait_idle(0ms));
    ops.release();
    ASSERT_TRUE(writer.wait_idle(LIMIT));
    EXPECT_EQ(writer.take_done().size(), 3U);
    EXPECT_EQ(writer.lost(), 0U);
}

TEST(RetainWriter, ALaterFileOfAnOwnerReplacesTheOneStillWaiting) {
    BlockingOps ops;
    RetainWriter writer(NO_DIRECTORY, ops);
    writer.submit("io", one_value(1), 1);
    ASSERT_TRUE(ops.wait_syncing());
    writer.submit("web", one_value(2), 2);
    writer.submit("web", one_value(3), 3);
    ops.release();
    ASSERT_TRUE(writer.wait_idle(LIMIT));
    const std::vector<RetainWriter::Done> done = writer.take_done();
    ASSERT_EQ(done.size(), 2U);
    EXPECT_EQ(done.at(0).owner, "io");
    EXPECT_EQ(done.at(1).owner, "web");
    EXPECT_EQ(done.at(1).token, 3U);
    EXPECT_TRUE(done.at(1).written);
    EXPECT_EQ(ops.renamed(), (std::vector<std::string>{"io.bin", "web.bin"}));
}

TEST(RetainWriter, AStopEndsAfterTheFileInFlightAndWritesNothingElse) {
    BlockingOps ops;
    auto writer = std::make_unique<RetainWriter>(NO_DIRECTORY, ops);
    writer->submit("io", one_value(1), 1);
    ASSERT_TRUE(ops.wait_syncing());
    writer->submit("web", one_value(2), 2);
    auto stopped =
        std::async(std::launch::async, [&writer] { writer.reset(); });
    // The destructor waits for the file in flight...
    EXPECT_EQ(stopped.wait_for(100ms), std::future_status::timeout);
    ops.release();
    // ...and only for that one.
    ASSERT_EQ(stopped.wait_for(LIMIT), std::future_status::ready);
    EXPECT_EQ(ops.renamed(), std::vector<std::string>{"io.bin"});
}

TEST(RetainWriter, ARemovalGoesThroughTheFileOperations) {
    BlockingOps ops;
    ops.release();
    RetainWriter writer(NO_DIRECTORY, ops);
    writer.submit_removal({"old.owner.bin"});
    ASSERT_TRUE(writer.wait_idle(LIMIT));
    const std::vector<RetainWriter::Done> done = writer.take_done();
    ASSERT_EQ(done.size(), 1U);
    EXPECT_TRUE(done.at(0).removal);
    EXPECT_TRUE(done.at(0).written);
    EXPECT_EQ(done.at(0).removed, std::vector<std::string>{"old.owner.bin"});
    EXPECT_EQ(ops.unlinked(), std::vector<std::string>{"old.owner.bin"});
}

}  // namespace
