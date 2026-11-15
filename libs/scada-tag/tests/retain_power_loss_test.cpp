// The atomic write of a retain file against a model of a power loss
// (contract, section 12; decision D-45). SIGKILL cannot show durability:
// the page cache survives the kill. Here the file system calls of
// write_retain_file go to a model that keeps, for every file, what was
// synced apart from what was written, and for the directory, the entries
// as last synced apart from the entries now. A power loss at any step then
// leaves, for the names, either set of entries, and for a file whose
// written data was not synced: nothing of it, a prefix, damaged bytes, or
// all of it. Each such state is written to a real directory and loaded.

#include "region_file.h"
#include "retain_file.h"
#include "temp_database.h"

#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/types.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

    // Each call counts as one step; once `crash_after` steps were made,
    // every later call fails and changes nothing: the machine is off.
    std::optional<std::size_t> crash_after;
    // The call named `fail_on` fails once with `fail_errno`.
    std::string fail_on;
    int fail_errno = 0;
    // Without the guarantee of fsync: a model of a writer that leaves it out.
    bool sync_does_nothing = false;

    std::vector<std::string> calls;
    std::map<std::string, std::size_t> entries;
    std::map<std::string, std::size_t> synced_entries;
    std::vector<Inode> inodes;

    // A file that is there and on disk, as before the write.
    void put(const std::string& name, const Bytes& bytes) {
        inodes.push_back({bytes, bytes});
        entries[name] = inodes.size() - 1;
        synced_entries = entries;
    }

    // One call. False if it must fail (crash or injected error).
    [[nodiscard]] int step(const std::string& call) {
        if (crash_after.has_value() && steps_ >= *crash_after) {
            return EIO;
        }
        ++steps_;
        calls.push_back(call);
        if (call == fail_on) {
            fail_on.clear();
            return fail_errno;
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
            Bytes& data = model_.inodes[inode_].written;
            data.insert(data.end(), bytes.begin(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(written));
            return 0;
        }
        int sync() noexcept override {
            if (const int error = model_.step("sync"); error != 0) {
                return error;
            }
            if (!model_.sync_does_nothing) {
                model_.inodes[inode_].synced = model_.inodes[inode_].written;
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
        if (entries.contains(name)) {
            return EEXIST;
        }
        inodes.push_back({});
        entries[name] = inodes.size() - 1;
        out = std::make_unique<File>(*this, inodes.size() - 1);
        return 0;
    }
    int rename(int /*dir_fd*/, const std::string& from,
               const std::string& to) noexcept override {
        if (const int error = step("rename"); error != 0) {
            return error;
        }
        const auto found = entries.find(from);
        if (found == entries.end()) {
            return ENOENT;
        }
        entries[to] = found->second;
        entries.erase(from);
        return 0;
    }
    int sync_directory(int /*dir_fd*/) noexcept override {
        if (const int error = step("sync_directory"); error != 0) {
            return error;
        }
        if (!sync_does_nothing) {
            synced_entries = entries;
        }
        return 0;
    }
    int unlink(int /*dir_fd*/, const std::string& name) noexcept override {
        if (const int error = step("unlink"); error != 0) {
            return error;
        }
        return entries.erase(name) == 1 ? 0 : ENOENT;
    }

private:
    std::size_t steps_ = 0;
};

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
            return Bytes(inode.written.begin(),
                         inode.written.begin() + static_cast<std::ptrdiff_t>(
                                                     inode.written.size() / 2));
        case Loss::DAMAGED: {
            Bytes bytes = inode.written;
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                bytes[i] ^= std::byte{0x5A};
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
    [[nodiscard]] std::vector<std::optional<Bytes>> states(
        const PowerLossModel& model) const {
        std::vector<std::optional<Bytes>> result;
        for (const auto* names : {&model.synced_entries, &model.entries}) {
            const auto found = names->find("io.bin");
            for (const Loss loss : LOSSES) {
                if (found == names->end()) {
                    result.emplace_back(std::nullopt);
                } else {
                    result.emplace_back(
                        after_loss(model.inodes.at(found->second), loss));
                }
            }
        }
        return result;
    }

    [[nodiscard]] int dir() const { return directory_.get(); }

    // The number of steps of one complete write.
    [[nodiscard]] std::size_t steps_of_a_write(bool with_old) {
        PowerLossModel model;
        if (with_old) {
            model.put("io.bin", encoded(values(OLD)));
        }
        std::string detail;
        EXPECT_TRUE(
            write_retain_file(dir(), "io", values(NEW), detail, model).ok());
        return model.calls.size();
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
    // unlink of a stale temporary file of this process id, then the write.
    ASSERT_GE(model.calls.size(), 6U);
    EXPECT_EQ(model.calls.front(), "unlink");
    EXPECT_EQ(model.calls[1], "create");
    EXPECT_EQ(model.calls[2], "set_mode");
    const std::size_t last = model.calls.size() - 1;
    for (std::size_t i = 3; i < last - 2; ++i) {
        EXPECT_EQ(model.calls[i], "write") << i;
    }
    EXPECT_EQ(model.calls[last - 2], "sync");
    EXPECT_EQ(model.calls[last - 1], "rename");
    EXPECT_EQ(model.calls[last], "sync_directory");
    EXPECT_EQ(model.entries.size(), 1U);
}

TEST_F(PowerLoss, ALossAtAnyStepLeavesTheOldOrTheNewValues) {
    const std::size_t steps = steps_of_a_write(true);
    for (std::size_t crash = 0; crash <= steps; ++crash) {
        PowerLossModel model;
        model.put("io.bin", encoded(values(OLD)));
        model.crash_after = crash;
        std::string detail;
        static_cast<void>(
            write_retain_file(dir(), "io", values(NEW), detail, model));
        for (const std::optional<Bytes>& state : states(model)) {
            const std::optional<std::uint64_t> level = load(state);
            ASSERT_TRUE(level == OLD || level == NEW)
                << "crash after step " << crash;
        }
    }
}

TEST_F(PowerLoss, WithoutAFormerFileALossLeavesNoFileOrTheNewOne) {
    const std::size_t steps = steps_of_a_write(false);
    for (std::size_t crash = 0; crash <= steps; ++crash) {
        PowerLossModel model;
        model.crash_after = crash;
        std::string detail;
        static_cast<void>(
            write_retain_file(dir(), "io", values(NEW), detail, model));
        for (const std::optional<Bytes>& state : states(model)) {
            ASSERT_TRUE(!state.has_value() || load(state) == NEW)
                << "crash after step " << crash;
        }
    }
}

// The model finds the fault it is there for: without fsync a loss after
// the rename can leave a file that does not load.
TEST_F(PowerLoss, TheModelCatchesAWriterWithoutFsync) {
    PowerLossModel model;
    model.put("io.bin", encoded(values(OLD)));
    model.sync_does_nothing = true;
    std::string detail;
    ASSERT_TRUE(
        write_retain_file(dir(), "io", values(NEW), detail, model).ok());
    bool unreadable = false;
    for (const std::optional<Bytes>& state : states(model)) {
        const std::optional<std::uint64_t> level = load(state);
        unreadable = unreadable || !(level == OLD || level == NEW);
    }
    EXPECT_TRUE(unreadable);
}

TEST_F(PowerLoss,
       AnErrorOnWriteFsyncOrRenameKeepsTheOldFileAndRemovesTheTemporary) {
    for (const std::string call :
         {"create", "set_mode", "write", "sync", "rename"}) {
        for (const int error : {ENOSPC, EIO}) {
            PowerLossModel model;
            model.put("io.bin", encoded(values(OLD)));
            model.fail_on = call;
            model.fail_errno = error;
            std::string detail;
            const scada::Status status =
                write_retain_file(dir(), "io", values(NEW), detail, model);
            ASSERT_FALSE(status.ok()) << call;
            EXPECT_EQ(status.code(), error == ENOSPC
                                         ? StatusCode::RESOURCE_EXHAUSTED
                                         : StatusCode::IO_ERROR)
                << call;
            // Only the old file is left, and it loads.
            ASSERT_EQ(model.entries.size(), 1U) << call;
            ASSERT_TRUE(model.entries.contains("io.bin")) << call;
            EXPECT_EQ(load(model.inodes.at(model.entries.at("io.bin")).written),
                      OLD);
            // The next write starts with a fresh file and works.
            ASSERT_TRUE(
                write_retain_file(dir(), "io", values(NEW), detail, model).ok())
                << call << ": " << detail;
            EXPECT_EQ(load(model.inodes.at(model.entries.at("io.bin")).written),
                      NEW);
            // Each fsync of the second write was on a file of its own.
            EXPECT_EQ(model.inodes.size(), call == "create" ? 2U : 3U) << call;
        }
    }
}

TEST_F(PowerLoss, AFailedDirectorySyncIsReported) {
    PowerLossModel model;
    model.fail_on = "sync_directory";
    model.fail_errno = EIO;
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
