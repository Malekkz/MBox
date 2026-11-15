#include "retain_keeper.h"

#include "header_view.h"
#include "layout.h"
#include "peer_regions.h"
#include "proc_region.h"
#include "region_file.h"
#include "retain_file.h"
#include "shared_memory.h"
#include "tag_record.h"

#include "scada/common/clock.h"
#include "scada/common/fd.h"
#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/generation_host.h"
#include "scada/tag/liveness.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/types.h"

#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::tag {

namespace {

constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t RETIRED_TIMEOUT_NS =
    std::int64_t{RETIRED_GENERATION_TIMEOUT_MS} * NS_PER_MS;

// True if `copy`, a record of a tag of type `type`, holds a value worth
// keeping (decision D-45): a value an owner may store, of quality GOOD or
// UNCERTAIN, not NOT_INIT.
bool holds_value(const RecordCopy& copy, TagType type) noexcept {
    const bool quality = copy.quality == to_raw(Quality::GOOD) ||
                         copy.quality == to_raw(Quality::UNCERTAIN);
    return quality &&
           (copy.quality_flags & to_raw(QualityFlag::NOT_INIT)) == 0 &&
           (type != TagType::BOOL || copy.value <= 1);
}

// The later of two identities, by generation number.
GenerationIdentity newer(const std::optional<GenerationIdentity>& kept,
                         const GenerationIdentity& seen) noexcept {
    return kept.has_value() && kept->generation >= seen.generation ? *kept
                                                                   : seen;
}

// True if a file marked FINAL for `mark` holds what its owner left in
// `generation`: that generation itself, or a later one.
bool covers(const std::optional<GenerationIdentity>& mark,
            const GenerationIdentity& generation) noexcept {
    return mark.has_value() &&
           (mark->generation > generation.generation || *mark == generation);
}

}  // namespace

RetainWriter::RetainWriter(int dir_fd, RetainFileOps& ops)
    : dir_fd_(dir_fd),
      ops_(ops),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

RetainWriter::~RetainWriter() {
    // The thread ends after the file it is writing, if any; files still
    // waiting are not written (`flush` is what writes them at a stop).
    thread_.request_stop();
}

void RetainWriter::submit(const std::string& owner, RetainFile file,
                          std::uint64_t token) {
    {
        const std::scoped_lock lock(mutex_);
        waiting_.insert_or_assign(owner, Job{std::move(file), token});
    }
    changed_.notify_all();
}

void RetainWriter::submit_removal(std::vector<std::string> names) {
    {
        const std::scoped_lock lock(mutex_);
        removals_.insert(removals_.end(), names.begin(), names.end());
    }
    changed_.notify_all();
}

std::vector<RetainWriter::Done> RetainWriter::take_done() {
    const std::scoped_lock lock(mutex_);
    std::vector<Done> done = std::move(done_);
    done_.clear();
    return done;
}

std::uint64_t RetainWriter::lost() const {
    const std::scoped_lock lock(mutex_);
    return lost_;
}

bool RetainWriter::wait_idle(std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, limit, [this] {
        return waiting_.empty() && removals_.empty() && !busy_;
    });
}

RetainWriter::Done RetainWriter::remove_files(
    const std::vector<std::string>& names) {
    Done done;
    done.removal = true;
    for (const std::string& name : names) {
        if (ops_.unlink(dir_fd_, name) == 0) {
            done.removed.push_back(name);
        }
    }
    // The removals are on disk only once the directory is.
    done.written = done.removed.empty() || ops_.sync_directory(dir_fd_) == 0;
    if (!done.written) {
        done.detail = "cannot flush the retain directory";
    }
    return done;
}

void RetainWriter::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    for (;;) {
        const bool ready = changed_.wait(lock, stop, [this] {
            return !waiting_.empty() || !removals_.empty();
        });
        if (!ready || stop.stop_requested()) {
            return;
        }
        const std::vector<std::string> removals = std::move(removals_);
        removals_.clear();
        std::optional<decltype(waiting_)::node_type> job;
        if (removals.empty()) {
            job = waiting_.extract(waiting_.begin());
        }
        busy_ = true;
        lock.unlock();

        Done done;
        try {
            if (job.has_value()) {
                done.owner = std::move(job->key());
                done.token = job->mapped().token;
                done.values = job->mapped().file.values.size();
                done.final = job->mapped().file.final;
                done.source = job->mapped().file.source;
                done.written =
                    write_retain_file(dir_fd_, done.owner, job->mapped().file,
                                      done.detail, ops_)
                        .ok();
            } else {
                done = remove_files(removals);
            }
        } catch (const std::exception& error) {
            // Nothing may leave the thread; a failed allocation is a failed
            // write, tried again later.
            done.written = false;
            done.detail = error.what();
        }

        lock.lock();
        busy_ = false;
        try {
            done_.push_back(std::move(done));
        } catch (const std::exception&) {
            // The result is lost; the keeper treats every file it waits
            // for as failed and writes it again.
            ++lost_;
        }
        changed_.notify_all();
    }
}

RetainKeeper::RetainKeeper(Key /*key*/, Fd directory, std::uint32_t tagd_uid,
                           std::int64_t interval_ns,
                           const ConfigHash& config_hash)
    : directory_(std::move(directory)),
      tagd_uid_(tagd_uid),
      interval_ns_(interval_ns),
      config_hash_(config_hash) {
    if (directory_.valid()) {
        writer_ = std::make_unique<RetainWriter>(directory_.get());
    }
}

RetainKeeper::~RetainKeeper() = default;

Result<std::unique_ptr<RetainKeeper>> RetainKeeper::open(
    const std::string& retain_dir, std::uint32_t tagd_uid, int base_fd,
    std::uint32_t interval_ms, const ConfigHash& config_hash,
    std::string& detail) {
    Fd directory;
    if (!retain_dir.empty()) {
        Result<Fd> opened =
            open_retain_directory(retain_dir, tagd_uid, base_fd, detail);
        if (!opened.ok()) {
            return opened.status();
        }
        directory = std::move(opened).value();
        static_cast<void>(remove_retain_temporaries(directory.get()));
    }
    return std::make_unique<RetainKeeper>(Key{}, std::move(directory), tagd_uid,
                                          std::int64_t{interval_ms} * NS_PER_MS,
                                          config_hash);
}

void RetainKeeper::describe(Watched& watched, bool retired,
                            std::int64_t now_mono_ns) const {
    const HeaderView& header = *watched.header;
    const HeaderContents& contents = header.contents();
    for (std::size_t i = 0; i < contents.owners.size(); ++i) {
        const OwnerInfo& info = contents.owners[i];
        WatchedOwner owner;
        owner.index = i;
        owner.name = info.name;
        owner.process_index = info.process_index;
        for (std::uint32_t slot = 0; slot < info.tag_count; ++slot) {
            const std::optional<TagEntry> entry = header.slot_entry(i, slot);
            if (!entry.has_value() || entry->type == TagType::STRING) {
                continue;
            }
            bool retain = (entry->flags & to_raw(TagFlag::RETAIN)) != 0;
            bool now = (entry->flags & to_raw(TagFlag::RETAIN_NOW)) != 0;
            // In a retired generation, a tag the current generation retains
            // counts too: an owner it moved to waits for this file
            // (decision D-45).
            if (retired && !retain && current_ != nullptr) {
                const std::optional<TagEntry> later =
                    current_->header->find_tag(entry->name);
                retain = later.has_value() &&
                         (later->flags & to_raw(TagFlag::RETAIN)) != 0;
                now = false;
            }
            if (retain) {
                owner.tags.push_back(
                    {std::string(entry->name), slot, entry->type, now});
            }
        }
        if (owner.tags.empty()) {
            continue;
        }
        owner.done = !persists();
        watched.peers->want(watched.dir_fd, watched.peers->tags_slot(i),
                            now_mono_ns);
        watched.owners.push_back(std::move(owner));
    }
    if (!retired) {
        return;
    }
    for (std::size_t i = 0; i < contents.processes.size(); ++i) {
        const ProcessInfo& process = contents.processes[i];
        WatchedProcess watched_process;
        watched_process.region.process = process.name;
        // Below MAX_PROCESSES, which fits 16 bits.
        watched_process.region.process_index = static_cast<std::uint16_t>(i);
        watched_process.region.generation = contents.generation;
        watched_process.region.queue_count = contents.queues.size();
        watched_process.region.uid = process.uid;
        watched_process.heartbeat_timeout_ms = process.heartbeat_timeout_ms;
        watched.processes.push_back(std::move(watched_process));
    }
}

Status RetainKeeper::watch_current(int dir_fd, std::uint64_t generation,
                                   std::string& detail) {
    Result<HeaderView> header =
        HeaderView::open(dir_fd, generation, tagd_uid_, detail);
    if (!header.ok()) {
        return header.status();
    }
    auto watched = std::make_unique<Watched>();
    watched->generation = generation;
    watched->dir_fd = dir_fd;
    watched->header = std::make_unique<HeaderView>(std::move(header).value());
    watched->identity = {generation,
                         watched->header->contents().created_time_ns};
    watched->peers = std::make_unique<PeerRegions>(watched->header->contents());
    current_ = std::move(watched);
    describe(*current_, false, monotonic_ns());
    return {};
}

std::optional<GenerationState> RetainKeeper::watch_retired(
    int base_fd, std::uint64_t generation, std::int64_t now_mono_ns) {
    auto watched = std::make_unique<Watched>();
    watched->generation = generation;
    watched->retired_mono_ns = now_mono_ns;
    watched->directory =
        open_at(base_fd, generation_dir_name(generation),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    watched->dir_fd = watched->directory.get();
    std::optional<GenerationState> state;
    if (watched->directory.valid()) {
        std::string ignored;
        Result<HeaderView> header =
            HeaderView::open(watched->dir_fd, generation, tagd_uid_, ignored);
        if (header.ok()) {
            state = header.value().state();
            watched->header =
                std::make_unique<HeaderView>(std::move(header).value());
            watched->identity = {generation,
                                 watched->header->contents().created_time_ns};
            watched->peers =
                std::make_unique<PeerRegions>(watched->header->contents());
            describe(*watched, true, now_mono_ns);
        }
    }
    retired_.push_back(std::move(watched));
    return state;
}

void RetainKeeper::load_watched(
    const Watched& watched,
    std::map<std::string, std::vector<std::string>, std::less<>>& own_names) {
    for (const WatchedOwner& owner : watched.owners) {
        std::vector<WantedValue> wanted;
        wanted.reserve(owner.tags.size());
        for (const RetainedTag& tag : owner.tags) {
            wanted.push_back({tag.name, tag.type});
        }
        const LoadedValues loaded =
            load_retained(directory_.get(), tagd_uid_, owner.name, wanted);
        OwnerState& state = states_[owner.name];
        for (const std::optional<RetainedValue>& value : loaded.values) {
            if (value.has_value()) {
                state.values.try_emplace(value->name, *value);
            }
        }
        if (loaded.own.has_value()) {
            state.has_file = true;
            if (loaded.own->final) {
                state.final_source =
                    newer(state.final_source, loaded.own->source);
                state.final_on_disk =
                    newer(state.final_on_disk, loaded.own->source);
            }
            std::vector<std::string>& names = own_names[owner.name];
            for (const RetainedValue& value : loaded.own->values) {
                names.push_back(value.name);
            }
        }
        for (const RejectedRetainFile& rejected : loaded.rejected) {
            if (rejected_seen_.insert(rejected.file).second) {
                pending_problems_.push_back(
                    {rejected.file, "ignored: " + rejected.detail});
            }
        }
    }
}

void RetainKeeper::load() {
    if (!persists()) {
        return;
    }
    std::map<std::string, std::vector<std::string>, std::less<>> own_names;
    if (current_ != nullptr) {
        load_watched(*current_, own_names);
    }
    for (const std::unique_ptr<Watched>& watched : retired_) {
        load_watched(*watched, own_names);
    }
    for (auto& [name, state] : states_) {
        // A value found only in another owner's file, or a name the owner
        // has nowhere any more: write the file anew.
        const auto names = own_names.find(name);
        const std::size_t on_file =
            names == own_names.end() ? 0 : names->second.size();
        const bool stale =
            names != own_names.end() &&
            std::ranges::any_of(names->second, [&](const std::string& tag) {
                return !state.values.contains(tag);
            });
        if (stale || state.values.size() != on_file) {
            state.dirty = true;
        }
    }
}

bool RetainKeeper::read_owner(const Watched& watched,
                              const WatchedOwner& owner) {
    if (owner.tags.empty() || watched.peers == nullptr) {
        return true;
    }
    const std::span<std::byte> tags =
        watched.peers->bytes(watched.peers->tags_slot(owner.index));
    if (tags.empty()) {
        return false;
    }
    OwnerState& state = states_[owner.name];
    for (const RetainedTag& tag : owner.tags) {
        const std::size_t offset =
            sizeof(RegionHeader) + (std::size_t{tag.slot} * sizeof(TagRecord));
        // The mapping has the size of the owner's tag count (PeerRegions
        // checks it), and every slot is below it.
        if (!fits(tags.size(), offset, sizeof(TagRecord))) {
            continue;
        }
        // The raw record, without the overlay of section 6.2: the value an
        // owner that has left wrote last is what is kept.
        RecordCopy copy;
        if (!read_record(shared_at<TagRecord>(tags, offset), copy) ||
            copy.type != to_raw(tag.type)) {
            continue;  // nothing learned: the record on file stays
        }
        if (!holds_value(copy, tag.type)) {
            // BAD or NOT_INIT, read consistently: the owner disowned the
            // value, and the tag starts again BAD with NOT_INIT (decision
            // D-45, reference 14.4).
            if (state.values.erase(tag.name) != 0) {
                state.dirty = true;
                state.urgent = state.urgent || tag.now;
            }
            continue;
        }
        const auto known = state.values.find(tag.name);
        if (known != state.values.end() && known->second.type == tag.type &&
            known->second.value == copy.value &&
            known->second.source_time_ns == copy.source_time_ns) {
            continue;
        }
        state.values.insert_or_assign(
            tag.name,
            RetainedValue{tag.name, tag.type, copy.value, copy.source_time_ns});
        state.dirty = true;
        state.urgent = state.urgent || tag.now;
    }
    return true;
}

void RetainKeeper::read_current() {
    for (const WatchedOwner& owner : current_->owners) {
        // Every check, not once a second: a region that has just appeared
        // is read at once, and so is one that was replaced.
        static_cast<void>(current_->peers->look_now(
            current_->dir_fd, current_->peers->tags_slot(owner.index)));
        static_cast<void>(read_owner(*current_, owner));
    }
}

bool RetainKeeper::current_has_region(const std::string& name) {
    if (current_ == nullptr) {
        return false;
    }
    const auto owner =
        std::ranges::find(current_->owners, name, &WatchedOwner::name);
    if (owner == current_->owners.end()) {
        return false;
    }
    const std::size_t slot = current_->peers->tags_slot(owner->index);
    static_cast<void>(current_->peers->look_now(current_->dir_fd, slot));
    return !current_->peers->bytes(slot).empty();
}

void RetainKeeper::submit(const std::string& name, std::int64_t now_mono_ns) {
    OwnerState& state = states_[name];
    state.dirty = false;
    state.urgent = false;
    state.last_write_mono_ns = now_mono_ns;
    if (!persists()) {
        state.done_token = ++state.token;
        return;
    }
    RetainFile file;
    file.final = state.final_source.has_value();
    if (file.final) {
        file.source = *state.final_source;
    } else if (current_ != nullptr) {
        file.source = current_->identity;
    }
    file.written_time_ns = realtime_ns();
    file.config_hash = config_hash_;
    file.values.reserve(state.values.size());
    for (const auto& [tag, value] : state.values) {
        file.values.push_back(value);
    }
    writer_->submit(name, std::move(file), ++state.token);
}

void RetainKeeper::collect(std::int64_t now_mono_ns, CheckReport& report) {
    if (!persists()) {
        return;
    }
    if (const std::uint64_t lost = writer_->lost(); lost != writer_lost_) {
        // A result the writer could not record: every file waited for is
        // taken as failed and written again.
        writer_lost_ = lost;
        for (auto& [name, state] : states_) {
            if (state.done_token != state.token) {
                state.done_token = state.token;
                state.dirty = true;
            }
        }
    }
    for (const RetainWriter::Done& done : writer_->take_done()) {
        if (done.removal) {
            report.retain_files_removed.insert(
                report.retain_files_removed.end(), done.removed.begin(),
                done.removed.end());
            if (!done.written) {
                report.retain_problems.push_back(
                    {"retain directory", done.detail});
            }
            continue;
        }
        OwnerState& state = states_[done.owner];
        state.done_token = std::max(state.done_token, done.token);
        if (done.written) {
            state.has_file = true;
            state.failing = false;
            if (done.final) {
                state.final_on_disk = newer(state.final_on_disk, done.source);
            }
            report.retain_written.push_back(
                {done.owner, done.values, done.final});
            continue;
        }
        // Only the newest file matters; an older one was replaced.
        if (done.token == state.token) {
            state.dirty = true;
            state.next_retry_mono_ns = now_mono_ns + RETAIN_RETRY_INTERVAL_NS;
            if (!state.failing) {
                state.failing = true;
                report.retain_problems.push_back(
                    {done.owner, "cannot write: " + done.detail});
            }
        }
    }
}

bool RetainKeeper::newer_region_exists(int base_fd, std::uint64_t generation,
                                       const std::string& owner,
                                       std::int64_t now_mono_ns) const {
    // The file already marks a newer generation as left.
    if (const auto state = states_.find(owner); state != states_.end()) {
        const std::optional<GenerationIdentity>& mark =
            state->second.final_on_disk;
        if (mark.has_value() && mark->generation > generation) {
            return true;
        }
    }
    std::string ignored;
    const Result<std::vector<std::string>> entries =
        list_directory(base_fd, ignored);
    if (!entries.ok()) {
        return false;
    }
    for (const std::string& entry : entries.value()) {
        const std::uint64_t newer =
            parse_generation_dir_name(entry).value_or(0);
        if (newer <= generation) {
            continue;
        }
        const Fd directory = open_at(
            base_fd, entry, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        Result<HeaderView> header =
            directory.valid()
                ? HeaderView::open(directory.get(), newer, tagd_uid_, ignored)
                : Result<HeaderView>(Status(StatusCode::NOT_FOUND, "gone"));
        if (!header.ok()) {
            continue;
        }
        const auto& owners = header.value().contents().owners;
        const auto found = std::ranges::find(owners, owner, &OwnerInfo::name);
        if (found == owners.end()) {
            continue;
        }
        PeerRegions peers(header.value().contents());
        const std::size_t slot =
            peers.tags_slot(static_cast<std::size_t>(found - owners.begin()));
        peers.want(directory.get(), slot, now_mono_ns);
        if (!peers.bytes(slot).empty()) {
            return true;
        }
    }
    return false;
}

bool RetainKeeper::has_left(Watched& watched, const WatchedOwner& owner,
                            std::int64_t now_mono_ns) {
    // Left: the owner stored a state other than RUNNING after its last
    // write, or holds its lock no more. A stale heartbeat alone is not
    // enough: a hung owner can go on writing (decision D-45).
    if (owner.process_index >= watched.processes.size()) {
        return true;
    }
    WatchedProcess& process = watched.processes[owner.process_index];
    return !proc_lock_held(watched.dir_fd,
                           proc_region_name(process.region.process)) ||
           process.probe.observe(watched.dir_fd, process.region,
                                 process.heartbeat_timeout_ms,
                                 now_mono_ns) == DownReason::NOT_RUNNING;
}

void RetainKeeper::finish(int base_fd, Watched& watched,
                          std::int64_t now_mono_ns) {
    const bool timed_out =
        now_mono_ns - watched.retired_mono_ns >= RETIRED_TIMEOUT_NS;
    if (watched.peers != nullptr) {
        static_cast<void>(watched.peers->refresh(watched.dir_fd, now_mono_ns));
    }
    for (WatchedOwner& owner : watched.owners) {
        if (owner.done) {
            continue;
        }
        OwnerState& state = states_[owner.name];
        const bool in_flight = state.done_token != state.token;
        // Decision D-45: once the owner has a region in the current
        // generation, which holds newer values, nothing is written for it
        // from an older one.
        if (!owner.final_submitted && current_has_region(owner.name)) {
            owner.done = true;
            continue;
        }
        if (owner.final_submitted) {
            owner.done = covers(state.final_on_disk, watched.identity) ||
                         (timed_out && !in_flight);
            if (!owner.done && !in_flight && !state.dirty) {
                state.dirty = true;  // the FINAL file failed: again
            }
            continue;
        }
        const bool left = has_left(watched, owner, now_mono_ns);
        if (left && newer_region_exists(base_fd, watched.generation, owner.name,
                                        now_mono_ns)) {
            // A newer generation holds newer values: nothing from this one.
            owner.done = true;
        } else if (left) {
            static_cast<void>(read_owner(watched, owner));
            state.final_source = newer(state.final_source, watched.identity);
            submit(owner.name, now_mono_ns);
            owner.final_submitted = true;
        } else if (timed_out) {
            // Still attached: what can be read is written, not marked FINAL.
            static_cast<void>(read_owner(watched, owner));
            if (state.dirty) {
                submit(owner.name, now_mono_ns);
            }
            owner.done = true;
        }
    }
}

void RetainKeeper::remove_done(int base_fd, std::int64_t now_mono_ns,
                               CheckReport& report) {
    std::erase_if(retired_, [&](const std::unique_ptr<Watched>& watched) {
        const bool timed_out =
            now_mono_ns - watched->retired_mono_ns >= RETIRED_TIMEOUT_NS;
        const bool owners_done = std::ranges::all_of(
            watched->owners,
            [](const WatchedOwner& owner) { return owner.done; });
        const std::string name = generation_dir_name(watched->generation);
        if (!owners_done || (!timed_out && has_live_process(base_fd, name))) {
            return false;
        }
        // Something in it cannot be removed (section 3.3, for example a
        // directory another user filled): it stays until the next boot.
        const bool give_up =
            now_mono_ns - watched->retired_mono_ns >= 2 * RETIRED_TIMEOUT_NS;
        RetiredGeneration retired{
            watched->generation, false, timed_out, {}, false};
        if (timed_out) {
            for (WatchedProcess& process : watched->processes) {
                if (!proc_lock_held(watched->dir_fd,
                                    proc_region_name(process.region.process))) {
                    continue;
                }
                static_cast<void>(process.probe.observe(
                    watched->dir_fd, process.region,
                    process.heartbeat_timeout_ms, now_mono_ns));
                const std::optional<ProcFields> fields =
                    read_proc_fields(process.probe.bytes());
                retired.still_attached.push_back(
                    process.region.process + " (pid " +
                    std::to_string(fields.has_value() ? fields->pid : 0) + ")");
            }
        }
        // Unlinked, never truncated: mappings that processes and observers
        // still hold stay valid, and go with them.
        retired.removed = remove_generation(base_fd, name);
        const bool removed = retired.removed;
        retired.abandoned = !removed && give_up;
        if (removed || retired.abandoned || !watched->removal_reported) {
            watched->removal_reported = true;
            report.retired.push_back(std::move(retired));
        }
        return removed || give_up;
    });
}

void RetainKeeper::remove_files_of_gone_owners() {
    if (gone_owners_removed_ || !persists() || !retired_.empty() ||
        current_ == nullptr) {
        return;
    }
    for (const WatchedOwner& owner : current_->owners) {
        const auto state = states_.find(owner.name);
        if (state != states_.end() &&
            (!state->second.values.empty() || state->second.dirty) &&
            (!state->second.has_file || state->second.dirty ||
             state->second.done_token != state->second.token)) {
            return;  // not every current owner has its file yet
        }
    }
    gone_owners_removed_ = true;
    std::string ignored;
    const Result<std::vector<std::string>> entries =
        list_directory(directory_.get(), ignored);
    if (!entries.ok()) {
        return;
    }
    std::vector<std::string> gone;
    for (const std::string& entry : entries.value()) {
        const std::optional<std::string_view> owner =
            owner_of_retain_file(entry);
        if (owner.has_value() &&
            std::ranges::find(current_->owners, *owner, &WatchedOwner::name) ==
                current_->owners.end()) {
            gone.push_back(entry);
        }
    }
    // Unlinked and the directory synced by the writer thread, not here.
    if (!gone.empty()) {
        writer_->submit_removal(std::move(gone));
    }
}

void RetainKeeper::finish_now(int base_fd, std::uint64_t generation,
                              std::int64_t now_mono_ns, CheckReport& report) {
    static_cast<void>(watch_retired(base_fd, generation, now_mono_ns));
    const std::unique_ptr<Watched> watched = std::move(retired_.back());
    retired_.pop_back();
    if (!persists() || watched->peers == nullptr) {
        return;
    }
    std::map<std::string, std::vector<std::string>, std::less<>> ignored;
    load_watched(*watched, ignored);
    for (const WatchedOwner& owner : watched->owners) {
        // Decision D-45: a newer generation that holds a region of the
        // owner holds newer values; nothing is written from this one.
        if (newer_region_exists(base_fd, generation, owner.name, now_mono_ns)) {
            continue;
        }
        static_cast<void>(read_owner(*watched, owner));
        OwnerState& state = states_[owner.name];
        state.final_source = newer(state.final_source, watched->identity);
        submit(owner.name, now_mono_ns);
    }
    // The generation is removed when this returns: its files first.
    wait_bounded(report);
    collect(now_mono_ns, report);
}

void RetainKeeper::check(int base_fd, std::int64_t now_mono_ns,
                         CheckReport& report) {
    report.retain_problems.insert(report.retain_problems.end(),
                                  pending_problems_.begin(),
                                  pending_problems_.end());
    pending_problems_.clear();
    collect(now_mono_ns, report);
    if (persists() && current_ != nullptr) {
        read_current();
    }
    for (const std::unique_ptr<Watched>& watched : retired_) {
        finish(base_fd, *watched, now_mono_ns);
    }
    for (auto& [name, state] : states_) {
        if (!state.dirty || now_mono_ns < state.next_retry_mono_ns) {
            continue;
        }
        if (state.urgent || !state.last_write_mono_ns.has_value() ||
            now_mono_ns - *state.last_write_mono_ns >= interval_ns_) {
            submit(name, now_mono_ns);
        }
    }
    remove_done(base_fd, now_mono_ns, report);
    remove_files_of_gone_owners();
}

void RetainKeeper::flush(CheckReport& report) {
    const std::int64_t now = monotonic_ns();
    if (!persists()) {
        return;
    }
    if (current_ != nullptr) {
        read_current();
    }
    for (auto& [name, state] : states_) {
        if (state.dirty) {
            submit(name, now);
        }
    }
    wait_bounded(report);
    collect(now, report);
    for (const auto& [name, state] : states_) {
        if (state.dirty || state.done_token != state.token) {
            report.retain_problems.push_back(
                {name, "retained values not saved at the stop"});
        }
    }
}

void RetainKeeper::wait_bounded(CheckReport& report) {
    if (!writer_->wait_idle(RETAIN_WAIT_LIMIT)) {
        report.retain_problems.push_back(
            {"retain writer", "files not written within " +
                                  std::to_string(RETAIN_WAIT_LIMIT.count()) +
                                  " ms"});
    }
}

void RetainKeeper::wait_written(CheckReport& report) {
    if (persists()) {
        wait_bounded(report);
        collect(monotonic_ns(), report);
    }
}

std::vector<std::uint64_t> RetainKeeper::retired() const {
    std::vector<std::uint64_t> generations;
    generations.reserve(retired_.size());
    for (const std::unique_ptr<Watched>& watched : retired_) {
        generations.push_back(watched->generation);
    }
    std::ranges::sort(generations);
    return generations;
}

}  // namespace scada::tag
