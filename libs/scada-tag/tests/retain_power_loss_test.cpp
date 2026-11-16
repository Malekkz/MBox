// The atomic write of a retain file against a model of a power loss
// (contract, section 12; decision D-45). SIGKILL cannot show durability:
// the page cache survives the kill. Here the file system calls of
// write_retain_file go to a model that keeps, for every file, what was
// synced apart from what was written, and for the directory, the entries
// as last synced apart from the entries now. A power loss at any step then
// leaves, for the names, either set of entries, and for a file whose
// written data was not synced: nothing of it, a prefix, damaged bytes, or
// all of it. Each such state is written to a real directory and loaded.

#include "retain_file.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace scada::tag;
using scada::Fd;
using scada::Result;
using scada::StatusCode;
using Bytes = std::vector<std::byte>;

// A model of one directory and its files.
class PowerLossModel final : public RetainFileOps {
public:
    struct Inode {
        Bytes synced;
        Bytes written;
    };

    // What the test makes go wrong.
    struct Faults {
        // Each call counts as one step; once `crash_after` steps were made,
        // every later call fails and changes nothing: the machine is off.
        std::optional<std::size_t> crash_after;
        // The call named `fail_on` fails once with `fail_errno`.
        std::string fail_on;
        int fail_errno = 0;
        // Without the guarantee of fsync: a writer that leaves it out.
        bool sync_does_nothing = false;
    };

    // The directory: its entries now and as last synced, the files, and the
    // calls made, in order.
    struct Disk {
        std::vector<std::string> calls;
        std::map<std::string, std::size_t> entries;
        std::map<std::string, std::size_t> synced_entries;
        std::vector<Inode> inodes;
    };

    [[nodiscard]] Faults& faults() noexcept { return faults_; }
    [[nodiscard]] Disk& disk() noexcept { return disk_; }
    [[nodiscard]] const Disk& disk() const noexcept { return disk_; }

    // A file that is there and on disk, as before the write.
    void put(const std::string& name, const Bytes& bytes) {
        disk_.inodes.push_back({bytes, bytes});
        disk_.entries[name] = disk_.inodes.size() - 1;
        disk_.synced_entries = disk_.entries;
    }

    // One call. False if it must fail (crash or injected error).
    [[nodiscard]] int step(const std::string& call) {
        if (faults_.crash_after.has_value() && steps_ >= *faults_.crash_after) {
            return EIO;
        }
        ++steps_;
        disk_.calls.push_back(call);
        if (call == faults_.fail_on) {
            faults_.fail_on.clear();
            return faults_.fail_errno;
        }
        return 0;
    }

    class File final : public RetainWriteFile {
    public:
        File(PowerLossModel& model, std::size_t inode)
            : model_(model), inode_(inode) {}
        int set_mode(mode_t /*mode*/) noexcept override {
            return model_.step("set_mode");
        }
        int write(std::span<const std::byte> bytes,
                  std::size_t& written) noexcept override {
            written = 0;
            if (const int error = model_.step("write"); error != 0) {
                return error;
            }
            // Short writes, so that the loop of the writer is used.
            constexpr std::size_t MOST = 37;
            written = std::min(bytes.size(), MOST);
            Bytes& data = model_.disk().inodes[inode_].written;
            data.insert(data.end(), bytes.begin(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(written));
            return 0;
        }
        int sync() noexcept override {
            if (const int error = model_.step("sync"); error != 0) {
                return error;
            }
            if (!model_.faults().sync_does_nothing) {
                model_.disk().inodes[inode_].synced =
                    model_.disk().inodes[inode_].written;
            }
            return 0;
        }

    private:
        PowerLossModel& model_;
        std::size_t inode_;
    };

    int create(int /*dir_fd*/, const std::string& name,
               std::unique_ptr<RetainWriteFile>& out) override {
        if (const int error = step("create"); error != 0) {
            return error;
        }
        if (disk_.entries.contains(name)) {
            return EEXIST;
        }
        disk_.inodes.push_back({});
        disk_.entries[name] = disk_.inodes.size() - 1;
        out = std::make_unique<File>(*this, disk_.inodes.size() - 1);
        return 0;
    }
    int rename(int /*dir_fd*/, const std::string& from,
               const std::string& target) noexcept override {
        if (const int error = step("rename"); error != 0) {
            return error;
        }
        const auto found = disk_.entries.find(from);
        if (found == disk_.entries.end()) {
            return ENOENT;
        }
        disk_.entries[target] = found->second;
        disk_.entries.erase(from);
        return 0;
    }
    int sync_directory(int /*dir_fd*/) noexcept override {
        if (const int error = step("sync_directory"); error != 0) {
            return error;
        }
        if (!faults_.sync_does_nothing) {
            disk_.synced_entries = disk_.entries;
        }
        return 0;
    }
    int unlink(int /*dir_fd*/, const std::string& name) noexcept override {
        if (const int error = step("unlink"); error != 0) {
            return error;
        }
        return disk_.entries.erase(name) == 1 ? 0 : ENOENT;
    }

private:
    Faults faults_;
    Disk disk_;
    std::size_t steps_ = 0;
};

// What damaged bytes are XORed with.
constexpr std::byte DAMAGE_PATTERN{0x5A};

// What a file whose written data was not all synced holds after the loss.
enum class Loss : std::uint8_t { NOTHING, PREFIX, DAMAGED, ALL };
constexpr std::array<Loss, 4> LOSSES = {Loss::NOTHING, Loss::PREFIX,
                                        Loss::DAMAGED, Loss::ALL};

Bytes after_loss(const PowerLossModel::Inode& inode, Loss loss) {
    if (inode.synced == inode.written) {
        return inode.synced;
    }
    switch (loss) {
        case Loss::NOTHING:
            return inode.synced;
        case Loss::PREFIX:
            return {inode.written.begin(),
                    inode.written.begin() +
                        static_cast<std::ptrdiff_t>(inode.written.size() / 2)};
        case Loss::DAMAGED: {
            Bytes bytes = inode.written;
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                bytes[i] ^= DAMAGE_PATTERN;
            }
            return bytes;
        }
        case Loss::ALL:
            return inode.written;
    }
    return {};
}

RetainFile values(std::uint64_t level) {
    RetainFile file;
    file.values = {{"plant.level", TagType::UINT64, level, 1},
                   {"plant.mode", TagType::INT64, level + 1, 2}};
    return file;
}

Bytes encoded(const RetainFile& file) {
    std::string detail;
    return encode_retain_file(file, detail).value();
}

constexpr std::uint64_t OLD = 10;
constexpr std::uint64_t NEW = 20;

class PowerLoss : public scada::tag::testing::TempDatabase {
protected:
    void SetUp() override {
        TempDatabase::SetUp();
        directory_ = scada::tag::testing::open_directory(base());
    }

    // Writes `bytes` as "io.bin" in the real directory, or removes it, and
    // reads it as attach would. The level read, or nothing if absent or
    // refused.
    [[nodiscard]] std::optional<std::uint64_t> load(
        const std::optional<Bytes>& bytes) {
        std::filesystem::remove(base() / "io.bin");
        if (bytes.has_value()) {
            std::ofstream out(base() / "io.bin", std::ios::binary);
            out.write(std::bit_cast<const char*>(bytes->data()),
                      static_cast<std::streamsize>(bytes->size()));
        }
        std::string detail;
        const Result<RetainFile> file =
            read_retain_file(directory_.get(), "io.bin", uid(), detail);
        if (!file.ok()) {
            return std::nullopt;
        }
        return find_retained(file.value(), "plant.level")->value;
    }

    // Every state a power loss can leave of `model` for "io.bin".
    [[nodiscard]] static std::vector<std::optional<Bytes>> states(
        const PowerLossModel& model) {
        std::vector<std::optional<Bytes>> result;
        for (const auto* names :
             {&model.disk().synced_entries, &model.disk().entries}) {
            const auto found = names->find("io.bin");
            for (const Loss loss : LOSSES) {
                if (found == names->end()) {
                    result.emplace_back(std::nullopt);
                } else {
                    result.emplace_back(after_loss(
                        model.disk().inodes.at(found->second), loss));
                }
            }
        }
        return result;
    }

    [[nodiscard]] int dir() const { return directory_.get(); }

    // True if every state `model` can leave after a loss loads, with the
    // old values or the new ones.
    [[nodiscard]] bool every_state_loads_old_or_new(
        const PowerLossModel& model) {
        return std::ranges::all_of(states(model), [&](const auto& state) {
            const std::optional<std::uint64_t> level = load(state);
            return level == OLD || level == NEW;
        });
    }

    // A write that fails at `call` with `error`: it reports the error, the
    // old file is the only one left and loads, and the next write starts
    // with a fresh file and works.
    [[nodiscard]] ::testing::AssertionResult fails_and_keeps_the_old_file(
        const std::string& call, int error) {
        PowerLossModel model;
        model.put("io.bin", encoded(values(OLD)));
        model.faults().fail_on = call;
        model.faults().fail_errno = error;
        std::string detail;
        const scada::Status status =
            write_retain_file(dir(), "io", values(NEW), detail, model);
        const StatusCode expected = error == ENOSPC
                                        ? StatusCode::RESOURCE_EXHAUSTED
                                        : StatusCode::IO_ERROR;
        if (status.ok() || status.code() != expected) {
            return ::testing::AssertionFailure() << call << ": wrong status";
        }
        const PowerLossModel::Disk& disk = model.disk();
        if (disk.entries.size() != 1 || !disk.entries.contains("io.bin") ||
            load(disk.inodes.at(disk.entries.at("io.bin")).written) != OLD) {
            return ::testing::AssertionFailure()
                   << call << ": the old file is not the one left";
        }
        if (!write_retain_file(dir(), "io", values(NEW), detail, model).ok() ||
            load(disk.inodes.at(disk.entries.at("io.bin")).written) != NEW) {
            return ::testing::AssertionFailure()
                   << call << ": the next write failed: " << detail;
        }
        // Each fsync of the second write was on a file of its own.
        const std::size_t files = call == "create" ? 2U : 3U;
        if (disk.inodes.size() != files) {
            return ::testing::AssertionFailure()
                   << call << ": " << disk.inodes.size() << " files";
        }
        return ::testing::AssertionSuccess();
    }

    // The number of steps of one complete write.
    [[nodiscard]] std::size_t steps_of_a_write(bool with_old) {
        PowerLossModel model;
        if (with_old) {
            model.put("io.bin", encoded(values(OLD)));
        }
        std::string detail;
        EXPECT_TRUE(
            write_retain_file(dir(), "io", values(NEW), detail, model).ok());
        return model.disk().calls.size();
    }

private:
    Fd directory_;
};

TEST_F(PowerLoss, TheCallsComeInTheOrderOfTheContract) {
    PowerLossModel model;
    model.put("io.bin", encoded(values(OLD)));
    std::string detail;
    ASSERT_TRUE(write_retain_file(dir(), "io", values(NEW), detail, model).ok())
        << detail;
    // unlink of a stale temporary file of this process id, then the write;
    // the model takes at most 37 bytes per write.
    const std::vector<std::string>& calls = model.disk().calls;
    constexpr std::size_t FIXED_CALLS = 6;
    ASSERT_GT(calls.size(), FIXED_CALLS);
    std::vector<std::string> expected{"unlink", "create", "set_mode"};
    expected.insert(expected.end(), calls.size() - FIXED_CALLS, "write");
    expected.insert(expected.end(), {"sync", "rename", "sync_directory"});
    EXPECT_EQ(calls, expected);
    EXPECT_EQ(model.disk().entries.size(), 1U);
}

TEST_F(PowerLoss, ALossAtAnyStepLeavesTheOldOrTheNewValues) {
    const std::size_t steps = steps_of_a_write(true);
    for (std::size_t crash = 0; crash <= steps; ++crash) {
        PowerLossModel model;
        model.put("io.bin", encoded(values(OLD)));
        model.faults().crash_after = crash;
        std::string detail;
        static_cast<void>(
            write_retain_file(dir(), "io", values(NEW), detail, model));
        ASSERT_TRUE(every_state_loads_old_or_new(model))
            << "crash after step " << crash;
    }
}

TEST_F(PowerLoss, WithoutAFormerFileALossLeavesNoFileOrTheNewOne) {
    const std::size_t steps = steps_of_a_write(false);
    for (std::size_t crash = 0; crash <= steps; ++crash) {
        PowerLossModel model;
        model.faults().crash_after = crash;
        std::string detail;
        static_cast<void>(
            write_retain_file(dir(), "io", values(NEW), detail, model));
        for (const std::optional<Bytes>& state : states(model)) {
            ASSERT_TRUE(!state.has_value() || load(state) == NEW)
                << "crash after step " << crash;
        }
    }
}

// Review of PR #69, point 6: once a write reported success, every state
// a loss can leave holds the new values; the old ones never come back.
TEST_F(PowerLoss, AfterASuccessfulWriteEveryStateLoadsTheNewValues) {
    for (const bool with_old : {true, false}) {
        PowerLossModel model;
        if (with_old) {
            model.put("io.bin", encoded(values(OLD)));
        }
        std::string detail;
        ASSERT_TRUE(
            write_retain_file(dir(), "io", values(NEW), detail, model).ok())
            << detail;
        for (const std::optional<Bytes>& state : states(model)) {
            EXPECT_EQ(load(state), NEW)
                << (with_old ? "with" : "without") << " a former file";
        }
    }
}

// The model finds the fault it is there for: without fsync a loss after
// the rename can leave a file that does not load.
TEST_F(PowerLoss, TheModelCatchesAWriterWithoutFsync) {
    PowerLossModel model;
    model.put("io.bin", encoded(values(OLD)));
    model.faults().sync_does_nothing = true;
    std::string detail;
    ASSERT_TRUE(
        write_retain_file(dir(), "io", values(NEW), detail, model).ok());
    EXPECT_FALSE(every_state_loads_old_or_new(model));
}

TEST_F(PowerLoss,
       AnErrorOnWriteFsyncOrRenameKeepsTheOldFileAndRemovesTheTemporary) {
    for (const std::string call :
         {"create", "set_mode", "write", "sync", "rename"}) {
        EXPECT_TRUE(fails_and_keeps_the_old_file(call, ENOSPC));
        EXPECT_TRUE(fails_and_keeps_the_old_file(call, EIO));
    }
}

TEST_F(PowerLoss, AFailedDirectorySyncIsReported) {
    PowerLossModel model;
    model.faults().fail_on = "sync_directory";
    model.faults().fail_errno = EIO;
    std::string detail;
    EXPECT_EQ(write_retain_file(dir(), "io", values(NEW), detail, model).code(),
              StatusCode::IO_ERROR);
}

TEST_F(PowerLoss, EveryTruncationAndDamageOfAFileIsRefused) {
    const Bytes bytes = encoded(values(NEW));
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        ASSERT_EQ(
            load(Bytes(bytes.begin(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(size))),
            std::nullopt)
            << size;
    }
    ASSERT_EQ(load(Bytes(bytes.size(), std::byte{0})), std::nullopt);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        Bytes damaged = bytes;
        damaged[i] ^= std::byte{1};
        ASSERT_EQ(load(damaged), std::nullopt) << i;
    }
    EXPECT_EQ(load(bytes), NEW);
}

TEST_F(PowerLoss, AFileWithASecondNameIsRefused) {
    ASSERT_EQ(load(encoded(values(NEW))), NEW);
    std::filesystem::create_hard_link(base() / "io.bin", base() / "other");
    std::string detail;
    EXPECT_EQ(read_retain_file(dir(), "io.bin", uid(), detail).status().code(),
              StatusCode::DATA_CORRUPT);
}

}  // namespace
