#include "scada/ctl/watch.h"

#include "scada/common/clock.h"
#include "scada/ctl/arguments.h"
#include "scada/ctl/database.h"
#include "scada/ctl/format.h"
#include "scada/ctl/scadactl.h"
#include "scada/ctl/view.h"
#include "scada/tag/scada_tag.h"

#include <poll.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX signal functions are declared here, not in <csignal>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace scada::ctl {

namespace {

// The longest wait between two looks at the generation, in ms: the
// observer calls scada_tag_status about once a second (contract 11.4).
constexpr int STATUS_INTERVAL_MS = 1000;
constexpr std::int64_t STATUS_INTERVAL_NS = 1'000'000'000;
// The most sources of one wait set (contract 8.4).
constexpr std::size_t SOURCES_PER_WAIT_SET = 127;

// Blocks SIGINT and SIGTERM in the calling thread and turns them into a
// readable descriptor; restores the signal mask when destroyed.
class StopSignals {
public:
    StopSignals()
        : set_(stop_set()),
          blocked_(::pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0),
          descriptor_(blocked_
                          ? ::signalfd(-1, &set_, SFD_CLOEXEC | SFD_NONBLOCK)
                          : -1) {}
    ~StopSignals() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        if (blocked_) {
            static_cast<void>(
                ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
        }
    }
    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;
    StopSignals(StopSignals&&) = delete;
    StopSignals& operator=(StopSignals&&) = delete;

    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

    // True if a stop signal arrived; it is taken.
    [[nodiscard]] bool arrived() const noexcept {
        signalfd_siginfo info{};
        return ::read(descriptor_, &info, sizeof(info)) ==
               static_cast<ssize_t>(sizeof(info));
    }

private:
    static sigset_t stop_set() noexcept {
        sigset_t set{};
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        return set;
    }

    // In this order: each is set from the ones before it.
    sigset_t set_{};
    sigset_t previous_{};
    bool blocked_ = false;
    int descriptor_ = -1;
};

// The subscription to one owner and the watched tags of that owner.
struct OwnerWatch {
    std::string name;
    scada_sub* sub = nullptr;
    std::vector<std::size_t> tags;  // indexes into Watch::tags_
};

// What a step of the watch leads to. RESTARTED: a generation change was
// handled; every owner, subscription and tag of before is gone.
enum class Step : std::uint8_t { GO_ON, RESTARTED, DONE, FAILED };

class Watch {
public:
    Watch(Database& database, const Arguments& args, std::ostream& out,
          std::ostream& err)
        : db_(database), args_(args), out_(out), err_(err) {}
    ~Watch() { close(); }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&&) = delete;
    Watch& operator=(Watch&&) = delete;

    int run();

private:
    // Finds the tags, subscribes to their owners, then shows what they
    // hold now: subscribed first, so that no change between the read and
    // the subscription is lost (contract 13, scada_sub_open).
    [[nodiscard]] bool open(std::string& detail);
    void close();
    [[nodiscard]] bool open_wait_sets(std::string& detail);

    void show_now(std::size_t tag);
    [[nodiscard]] Step show_change(const scada_change& change);
    // Shows what every subscription has; GO_ON also after a restart.
    [[nodiscard]] Step drain_all();
    // Shows what `owner` has. RESTARTED: `owner` no longer exists.
    [[nodiscard]] Step drain(OwnerWatch& owner);
    [[nodiscard]] Step check_generation(bool now);
    [[nodiscard]] Step wait(const StopSignals& stop);
    void emit(const std::string& line);

    Database& db_;
    const Arguments& args_;
    std::ostream& out_;
    std::ostream& err_;

    std::vector<TagRef> tags_;
    // For every tag, `update_mono_ns` of what was last shown: a change
    // that is not newer is dropped (contract 8.3).
    std::vector<std::int64_t> shown_mono_;
    std::map<std::uint32_t, std::size_t> by_handle_;
    std::vector<OwnerWatch> owners_;
    std::vector<scada_waitset*> wait_sets_;
    std::vector<int> descriptors_;
    std::uint64_t changes_shown_ = 0;
    std::int64_t next_status_mono_ns_ = 0;
};

void Watch::emit(const std::string& line) {
    out_ << line << std::flush;
}

bool Watch::open(std::string& detail) {
    tags_ = db_.find(args_.pattern);
    if (tags_.empty()) {
        detail = "no tag matches ";
        append_terminal_safe(detail, args_.pattern.text());
        return false;
    }
    shown_mono_.assign(tags_.size(), 0);
    std::map<std::uint16_t, std::size_t> owner_of;
    for (std::size_t index = 0; index < tags_.size(); ++index) {
        by_handle_[tags_[index].handle] = index;
        const auto [where, added] =
            owner_of.try_emplace(tags_[index].owner_index, owners_.size());
        if (added) {
            owners_.push_back(
                {.name = tags_[index].owner, .sub = nullptr, .tags = {}});
        }
        owners_.at(where->second).tags.push_back(index);
    }
    for (OwnerWatch& owner : owners_) {
        if (scada_sub_open(db_.ctx(), owner.name.c_str(), &owner.sub) !=
            SCADA_OK) {
            detail = "cannot watch the changes of owner ";
            append_terminal_safe(detail, owner.name);
            return false;
        }
    }
    if (!open_wait_sets(detail)) {
        return false;
    }
    for (std::size_t index = 0; index < tags_.size(); ++index) {
        show_now(index);
    }
    return true;
}

bool Watch::open_wait_sets(std::string& detail) {
    for (std::size_t first = 0; first < owners_.size();
         first += SOURCES_PER_WAIT_SET) {
        scada_waitset* set = nullptr;
        if (scada_waitset_create(db_.ctx(), &set) != SCADA_OK) {
            detail = "cannot create a wait set";
            return false;
        }
        wait_sets_.push_back(set);
        const std::size_t last =
            std::min(owners_.size(), first + SOURCES_PER_WAIT_SET);
        for (std::size_t index = first; index < last; ++index) {
            if (scada_waitset_add_sub(set, owners_[index].sub) != SCADA_OK) {
                detail = "cannot wait on a change ring";
                return false;
            }
        }
        int descriptor = -1;
        if (scada_waitset_fd(set, &descriptor) != SCADA_OK) {
            detail = "cannot wait on the change rings";
            return false;
        }
        descriptors_.push_back(descriptor);
    }
    return true;
}

void Watch::close() {
    for (scada_waitset* set : wait_sets_) {
        scada_waitset_destroy(set);
    }
    for (const OwnerWatch& owner : owners_) {
        scada_sub_close(owner.sub);
    }
    wait_sets_.clear();
    descriptors_.clear();
    owners_.clear();
    by_handle_.clear();
    tags_.clear();
    shown_mono_.clear();
}

void Watch::show_now(std::size_t tag) {
    const TagRow row = db_.read(tags_.at(tag));
    if (row.read_code == SCADA_OK) {
        shown_mono_.at(tag) = row.value.update_mono_ns;
    }
    std::string line;
    if (args_.json) {
        render_watch_json(line, WatchEvent::NOW, row);
    } else {
        render_watch_text(line, WatchEvent::NOW, row);
    }
    emit(line);
}

Step Watch::show_change(const scada_change& change) {
    const auto found = by_handle_.find(change.handle);
    if (found == by_handle_.end()) {
        return Step::GO_ON;  // a tag of the owner that does not match
    }
    const std::size_t tag = found->second;
    if (change.update_mono_ns <= shown_mono_.at(tag)) {
        return Step::GO_ON;  // older than what was read after a gap
    }
    shown_mono_.at(tag) = change.update_mono_ns;
    TagRow row;
    row.name = tags_.at(tag).name;
    row.owner = tags_.at(tag).owner;
    row.type = change.type;
    row.value.type = change.type;
    row.value.quality = change.quality;
    row.value.quality_flags = change.quality_flags;
    row.value.value = change.value;
    row.value.source_time_ns = change.source_time_ns;
    row.value.update_mono_ns = change.update_mono_ns;
    row.value.age_ns =
        std::max<std::int64_t>(0, monotonic_ns() - change.update_mono_ns);
    if (change.type == SCADA_TYPE_STRING) {
        // The ring holds the slot, not the text: the text now. It may be
        // that of a later write of the same tag.
        row.text = db_.read(tags_.at(tag)).text;
    }
    std::string line;
    if (args_.json) {
        render_watch_json(line, WatchEvent::CHANGE, row);
    } else {
        render_watch_text(line, WatchEvent::CHANGE, row);
    }
    emit(line);
    ++changes_shown_;
    return args_.count.has_value() && changes_shown_ >= *args_.count
               ? Step::DONE
               : Step::GO_ON;
}

Step Watch::drain(OwnerWatch& owner) {
    for (;;) {
        scada_change change{};
        const int code = scada_sub_next(owner.sub, &change, 0);
        if (code == SCADA_E_TIMEOUT) {
            return Step::GO_ON;
        }
        if (code == SCADA_OK) {
            if (show_change(change) == Step::DONE) {
                return Step::DONE;
            }
            continue;
        }
        if (code == SCADA_E_GAP) {
            scada_sub_stats_t stats{};
            static_cast<void>(scada_sub_stats(owner.sub, &stats));
            std::string line;
            if (args_.json) {
                render_gap_json(line, owner.name, stats.last_gap);
            } else {
                render_gap_text(line, owner.name, stats.last_gap);
            }
            emit(line);
            // A gap may come with a generation change; look before the
            // tags are read again. After a restart `owner` is gone.
            if (const Step step = check_generation(true); step != Step::GO_ON) {
                return step;
            }
            for (const std::size_t tag : owner.tags) {
                show_now(tag);
            }
            continue;
        }
        if (code == SCADA_E_LAYOUT) {
            continue;  // an entry that is no valid change, skipped
        }
        err_ << "scadactl: reading the changes of " << owner.name
             << " failed (code " << code << ")\n";
        return Step::FAILED;
    }
}

Step Watch::check_generation(bool now) {
    const std::int64_t mono = monotonic_ns();
    if (!now && mono < next_status_mono_ns_) {
        return Step::GO_ON;
    }
    next_status_mono_ns_ = mono + STATUS_INTERVAL_NS;
    scada_status status{};
    if (db_.status(status) != SCADA_E_RETIRING) {
        return Step::GO_ON;
    }
    close();
    std::string detail;
    if (!db_.reopen(detail)) {
        err_ << "scadactl: " << detail << "\n";
        return Step::FAILED;
    }
    static_cast<void>(db_.status(status));
    std::string line;
    if (args_.json) {
        render_generation_json(line, status.generation);
    } else {
        render_generation_text(line, status.generation);
    }
    emit(line);
    if (!open(detail)) {
        err_ << "scadactl: " << detail << "\n";
        return Step::FAILED;
    }
    return Step::RESTARTED;
}

Step Watch::wait(const StopSignals& stop) {
    std::vector<pollfd> polled;
    polled.push_back({.fd = stop.descriptor(), .events = POLLIN, .revents = 0});
    for (const int descriptor : descriptors_) {
        polled.push_back({.fd = descriptor, .events = POLLIN, .revents = 0});
    }
    const int ready = ::poll(polled.data(), polled.size(), STATUS_INTERVAL_MS);
    if (ready < 0 && errno != EINTR) {
        err_ << "scadactl: waiting for changes failed\n";
        return Step::FAILED;
    }
    if ((polled.front().revents & POLLIN) != 0 && stop.arrived()) {
        return Step::DONE;
    }
    for (std::size_t index = 1; index < polled.size(); ++index) {
        if ((polled[index].revents & POLLIN) != 0) {
            // Takes the count, so that the descriptor is not readable
            // again for the same wake. A failed read (another thread took
            // it first, EAGAIN) only means one more look at the rings.
            std::array<std::byte, sizeof(std::uint64_t)> counter{};
            if (::read(polled[index].fd, counter.data(), counter.size()) < 0 &&
                errno != EAGAIN) {
                err_ << "scadactl: waiting for changes failed\n";
                return Step::FAILED;
            }
        }
    }
    return Step::GO_ON;
}

int Watch::run() {
    const StopSignals stop;
    if (stop.descriptor() < 0) {
        err_ << "scadactl: cannot handle SIGINT and SIGTERM\n";
        return EXIT_ERROR;
    }
    std::string detail;
    if (!open(detail)) {
        err_ << "scadactl: " << detail << "\n";
        return EXIT_ERROR;
    }
    next_status_mono_ns_ = monotonic_ns() + STATUS_INTERVAL_NS;
    for (;;) {
        // Changes that came while the current values were read are shown
        // before the first wait.
        if (const Step step = drain_all(); step != Step::GO_ON) {
            return step == Step::DONE ? EXIT_OK : EXIT_ERROR;
        }
        const Step waited = wait(stop);
        if (waited != Step::GO_ON) {
            return waited == Step::DONE ? EXIT_OK : EXIT_ERROR;
        }
        if (check_generation(false) == Step::FAILED) {
            return EXIT_ERROR;
        }
    }
}

Step Watch::drain_all() {
    // By index, not by iterator: a generation change found in `drain`
    // replaces `owners_`, and then the loop must end at once.
    // NOLINTNEXTLINE(modernize-loop-convert): a range loop would go on over a replaced vector
    for (std::size_t index = 0; index < owners_.size(); ++index) {
        const Step step = drain(owners_[index]);
        if (step == Step::RESTARTED) {
            return Step::GO_ON;
        }
        if (step != Step::GO_ON) {
            return step;
        }
    }
    return Step::GO_ON;
}

}  // namespace

int watch(Database& database, const Arguments& args, std::ostream& out,
          std::ostream& err) {
    Watch watching(database, args, out, err);
    return watching.run();
}

}  // namespace scada::ctl
