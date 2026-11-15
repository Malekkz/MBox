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
// observer calls scada_tag_status at least once a second (contract 11.4),
// and before every line it shows.
constexpr int STATUS_INTERVAL_MS = 1000;
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
// handled; every owner, subscription and tag of before is gone, and the
// lines of the new start are pending.
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
    // Takes what every subscription has into the pending lines. DONE when
    // the pending changes reach --count.
    [[nodiscard]] Step drain_all();
    [[nodiscard]] Step drain(OwnerWatch& owner);
    // Writes the pending lines, but only after scada_tag_status says that
    // the generation they were read from is still the current one: a line
    // is never shown for a retired generation (contract 11.4). If it was
    // retired, the lines are dropped and the watch starts over in the new
    // generation (RESTARTED: the lines of the new start are pending). A
    // change of the state of scada-tagd is shown first.
    [[nodiscard]] Step publish();
    // Observes again and opens the watch on the new generation.
    [[nodiscard]] Step restart();
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
    // Lines read but not yet shown, and how many changes they hold.
    std::string pending_;
    std::uint64_t pending_changes_ = 0;
    std::uint64_t changes_shown_ = 0;
    // Whether the last line about scada-tagd said it is down.
    bool tagd_down_shown_ = false;
};

void Watch::emit(const std::string& line) {
    pending_.append(line);
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
    ++pending_changes_;
    return args_.count.has_value() &&
                   changes_shown_ + pending_changes_ >= *args_.count
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
            // A gap may come with a generation change: `publish` looks
            // before these lines are shown.
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

Step Watch::publish() {
    scada_status status{};
    const int code = db_.status(status);
    if (code == SCADA_E_RETIRING) {
        pending_.clear();
        pending_changes_ = 0;
        return restart();
    }
    const bool tagd_down = code == SCADA_E_TAGD_DOWN;
    std::string lines;
    if (tagd_down != tagd_down_shown_) {
        if (args_.json) {
            render_tagd_json(lines, tagd_down);
        } else {
            render_tagd_text(lines, tagd_down);
        }
        tagd_down_shown_ = tagd_down;
    }
    lines.append(pending_);
    if (!lines.empty()) {
        out_ << lines << std::flush;
    }
    changes_shown_ += pending_changes_;
    pending_.clear();
    pending_changes_ = 0;
    return Step::GO_ON;
}

Step Watch::restart() {
    close();
    std::string detail;
    if (!db_.reopen(detail)) {
        err_ << "scadactl: " << detail << "\n";
        return Step::FAILED;
    }
    scada_status status{};
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
    for (;;) {
        // Changes that came while the current values were read are taken
        // before the first wait.
        const Step drained = drain_all();
        if (drained == Step::FAILED) {
            return EXIT_ERROR;
        }
        const Step published = publish();
        if (published == Step::FAILED) {
            return EXIT_ERROR;
        }
        if (published == Step::RESTARTED) {
            continue;  // the new start is pending: publish it first
        }
        if (drained == Step::DONE) {
            return EXIT_OK;
        }
        const Step waited = wait(stop);
        if (waited != Step::GO_ON) {
            return waited == Step::DONE ? EXIT_OK : EXIT_ERROR;
        }
    }
}

Step Watch::drain_all() {
    for (OwnerWatch& owner : owners_) {
        if (const Step step = drain(owner); step != Step::GO_ON) {
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
