#include "scada/tag/tag_set.h"

#include "layout.h"
#include "resolved_tag_set.h"
#include "sha256.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/handle.h"
#include "scada/tag/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

namespace {

// With at most MAX_TAGS tags in a generation, no owner can exceed its own
// limit, and every position in a list fits the index type the layout gives
// it. The casts below rely on these.
static_assert(MAX_TAGS <= MAX_TAGS_PER_OWNER);
static_assert(MAX_PROCESSES <= std::numeric_limits<std::uint16_t>::max());
static_assert(MAX_QUEUES <= std::numeric_limits<std::uint16_t>::max());

constexpr Status INVALID_NAME{StatusCode::INVALID_ARGUMENT,
                              "tag set: invalid name"};
constexpr Status DUPLICATE_NAME{StatusCode::ALREADY_EXISTS,
                                "tag set: duplicate name"};
constexpr Status UNKNOWN_REFERENCE{StatusCode::NOT_FOUND,
                                   "tag set: unknown process or owner"};
constexpr Status INVALID_VALUE{StatusCode::INVALID_ARGUMENT,
                               "tag set: invalid value"};
constexpr Status LIMIT_EXCEEDED{StatusCode::OUT_OF_RANGE,
                                "tag set: limit of the contract exceeded"};

// The lists of a tag set, as they are named in messages.
constexpr std::string_view PROCESS = "process";
constexpr std::string_view OWNER = "owner";
constexpr std::string_view QUEUE = "queue";
constexpr std::string_view TAG = "tag";

// Returns `text` in quotes. Used only for names that passed the grammar, so
// that nothing from outside reaches a message unchecked.
std::string quoted(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result += '\'';
    result += text;
    result += '\'';
    return result;
}

// "<kind> '<name>': <what>".
std::string about(std::string_view kind, std::string_view name,
                  std::string_view what) {
    std::string result(kind);
    result += ' ';
    result += quoted(name);
    result += ": ";
    result += what;
    return result;
}

// The positions of `definitions`, sorted by name.
template <typename Definition>
std::vector<std::size_t> order_by_name(
    const std::vector<Definition>& definitions) {
    std::vector<std::size_t> order(definitions.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(
        order, [&definitions](std::size_t left, std::size_t right) {
            return definitions[left].name < definitions[right].name;
        });
    return order;
}

// Returns the position in `order` of the entry named `name`, or nothing.
// `order` is `order_by_name(definitions)`.
template <typename Definition>
std::optional<std::size_t> find_by_name(
    const std::vector<Definition>& definitions,
    const std::vector<std::size_t>& order, std::string_view name) {
    const auto found = std::ranges::lower_bound(
        order, name, std::less<>{},
        [&definitions](std::size_t position) -> std::string_view {
            return definitions[position].name;
        });
    if (found == order.end() || definitions[*found].name != name) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - order.begin());
}

// Checks the count, the names and their uniqueness for one list.
template <typename Definition>
Status check_list(const std::vector<Definition>& definitions,
                  const std::vector<std::size_t>& order, std::string_view kind,
                  std::size_t max_count, std::size_t max_name_length,
                  std::string& detail) {
    if (definitions.size() > max_count) {
        detail = "the tag set has " + std::to_string(definitions.size()) + " " +
                 std::string(kind) + " entries; the limit is " +
                 std::to_string(max_count);
        return LIMIT_EXCEEDED;
    }
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (!is_valid_name(definitions[i].name, max_name_length)) {
            // The name itself is not repeated: it failed the grammar.
            detail = std::string(kind) + " at position " + std::to_string(i) +
                     " (from 0): the name must be 1 to " +
                     std::to_string(max_name_length) + " bytes of [a-z0-9_.-]";
            return INVALID_NAME;
        }
    }
    const auto duplicate = std::ranges::adjacent_find(
        order, [&definitions](std::size_t left, std::size_t right) {
            return definitions[left].name == definitions[right].name;
        });
    if (duplicate != order.end()) {
        detail = about(kind, definitions[*duplicate].name,
                       "the name is used more than once");
        return DUPLICATE_NAME;
    }
    return {};
}

// Resolves the process named `name`, which the entry `kind` `entry` refers
// to as its `role`.
Result<std::uint16_t> find_process(const TagSet& tag_set,
                                   const ResolvedTagSet& resolved,
                                   std::string_view name, std::string_view kind,
                                   std::string_view entry,
                                   std::string_view role, std::string& detail) {
    const auto position =
        find_by_name(tag_set.processes, resolved.processes, name);
    if (!position.has_value()) {
        std::string what(role);
        what += is_valid_name(name, MAX_SHORT_NAME_LENGTH)
                    ? " " + quoted(name) + " is not a process of the tag set"
                    : " is not a valid process name";
        detail = about(kind, entry, what);
        return UNKNOWN_REFERENCE;
    }
    // Below MAX_PROCESSES, which fits 16 bits.
    return static_cast<std::uint16_t>(*position);
}

Status resolve_processes(const TagSet& tag_set, ResolvedTagSet& resolved,
                         std::string& detail) {
    resolved.processes = order_by_name(tag_set.processes);
    if (const Status status =
            check_list(tag_set.processes, resolved.processes, PROCESS,
                       MAX_PROCESSES, MAX_SHORT_NAME_LENGTH, detail);
        !status.ok()) {
        return status;
    }
    for (const ProcessDefinition& process : tag_set.processes) {
        if (process.uid > MAX_UID) {
            detail = about(
                PROCESS, process.name,
                "uid " + std::to_string(process.uid) + " is not a user id");
            return INVALID_VALUE;
        }
        if (process.heartbeat_timeout_ms == 0) {
            detail = about(PROCESS, process.name,
                           "heartbeat_timeout_ms must be at least 1");
            return INVALID_VALUE;
        }
    }
    return {};
}

// The rule of contract section 2: the only owner of a process is named like
// the process; several owners are named `<process>.<writer>`.
Status check_owner_names(const TagSet& tag_set, const ResolvedTagSet& resolved,
                         std::string& detail) {
    std::vector<std::size_t> owners_of_process(resolved.processes.size(), 0);
    for (const ResolvedOwner& owner : resolved.owners) {
        ++owners_of_process.at(owner.process_index);
    }
    for (const ResolvedOwner& owner : resolved.owners) {
        const OwnerDefinition& definition = tag_set.owners.at(owner.definition);
        const std::string_view name = definition.name;
        const std::string_view process = definition.process;
        if (owners_of_process.at(owner.process_index) == 1) {
            if (name != process) {
                detail = about(OWNER, name,
                               "the only owner of process " + quoted(process) +
                                   " must have the name of the process");
                return INVALID_NAME;
            }
            continue;
        }
        const bool has_writer_part = name.size() > process.size() + 1 &&
                                     name.starts_with(process) &&
                                     name.at(process.size()) == '.';
        if (!has_writer_part) {
            detail = about(OWNER, name,
                           "process " + quoted(process) +
                               " has several owners, so each must be named "
                               "<process>.<writer>");
            return INVALID_NAME;
        }
    }
    return {};
}

Status resolve_owners(const TagSet& tag_set, ResolvedTagSet& resolved,
                      std::string& detail) {
    const std::vector<std::size_t> order = order_by_name(tag_set.owners);
    if (const Status status =
            check_list(tag_set.owners, order, OWNER, MAX_OWNERS,
                       MAX_SHORT_NAME_LENGTH, detail);
        !status.ok()) {
        return status;
    }
    resolved.owners.reserve(order.size());
    for (const std::size_t position : order) {
        const OwnerDefinition& owner = tag_set.owners[position];
        const Result<std::uint16_t> process =
            find_process(tag_set, resolved, owner.process, OWNER, owner.name,
                         "process", detail);
        if (!process.ok()) {
            return process.status();
        }
        if (!std::has_single_bit(owner.change_ring_capacity) ||
            owner.change_ring_capacity < MIN_CHANGE_RING_CAPACITY ||
            owner.change_ring_capacity > MAX_CHANGE_RING_CAPACITY) {
            detail =
                about(OWNER, owner.name,
                      "change_ring_capacity " +
                          std::to_string(owner.change_ring_capacity) +
                          " must be a power of two from " +
                          std::to_string(MIN_CHANGE_RING_CAPACITY) + " to " +
                          std::to_string(MAX_CHANGE_RING_CAPACITY));
            return LIMIT_EXCEEDED;
        }
        resolved.owners.push_back(ResolvedOwner{
            .definition = position, .process_index = process.value()});
    }
    return check_owner_names(tag_set, resolved, detail);
}

// Returns the kind that the prefix of the queue name `name` stands for, or
// nothing if the name has none of the prefixes or nothing after it.
std::optional<QueueKind> queue_kind_of(std::string_view name) noexcept {
    struct Prefix {
        std::string_view text;
        QueueKind kind = QueueKind::EVENTS;
    };
    static constexpr std::array PREFIXES{
        Prefix{"events.", QueueKind::EVENTS},
        Prefix{"cmd.", QueueKind::CMD},
        Prefix{"result.", QueueKind::RESULT},
    };
    for (const Prefix& prefix : PREFIXES) {
        if (name.size() > prefix.text.size() && name.starts_with(prefix.text)) {
            return prefix.kind;
        }
    }
    return std::nullopt;
}

Status resolve_queues(const TagSet& tag_set, ResolvedTagSet& resolved,
                      std::string& detail) {
    const std::vector<std::size_t> order = order_by_name(tag_set.queues);
    if (const Status status =
            check_list(tag_set.queues, order, QUEUE, MAX_QUEUES,
                       MAX_SHORT_NAME_LENGTH, detail);
        !status.ok()) {
        return status;
    }
    resolved.queues.reserve(order.size());
    for (const std::size_t position : order) {
        const QueueDefinition& queue = tag_set.queues[position];
        const std::optional<QueueKind> kind = queue_kind_of(queue.name);
        if (!kind.has_value()) {
            detail = about(QUEUE, queue.name,
                           "the name must start with events., cmd. or "
                           "result. and continue after it");
            return INVALID_NAME;
        }
        const Result<std::uint16_t> producer =
            find_process(tag_set, resolved, queue.producer, QUEUE, queue.name,
                         "producer", detail);
        if (!producer.ok()) {
            return producer.status();
        }
        const Result<std::uint16_t> consumer =
            find_process(tag_set, resolved, queue.consumer, QUEUE, queue.name,
                         "consumer", detail);
        if (!consumer.ok()) {
            return consumer.status();
        }
        if (producer.value() == consumer.value()) {
            // A queue is between two processes (contract, section 9); with
            // one at both ends its side could not be told at open.
            detail = about(QUEUE, queue.name,
                           "producer and consumer must be different "
                           "processes");
            return INVALID_VALUE;
        }
        if (!std::has_single_bit(queue.capacity)) {
            detail = about(QUEUE, queue.name,
                           "capacity " + std::to_string(queue.capacity) +
                               " must be a power of two");
            return INVALID_VALUE;
        }
        resolved.queues.push_back(
            ResolvedQueue{.definition = position,
                          .kind = *kind,
                          .producer_process = producer.value(),
                          .consumer_process = consumer.value()});
    }
    return {};
}

// Checks the type and the flags of `tag` and returns its TagFlag bits.
Result<std::uint8_t> tag_flags(const TagDefinition& tag, std::string& detail) {
    if (!tag_type_from_raw(to_raw(tag.type)).has_value()) {
        detail = about(TAG, tag.name, "the type is not a tag type");
        return INVALID_VALUE;
    }
    if (tag.retain_now && !tag.retain) {
        detail = about(TAG, tag.name, "retain_now needs retain");
        return INVALID_VALUE;
    }
    if (tag.retain && tag.type == TagType::STRING) {
        // A retain file holds 64-bit values only (decision D-43).
        detail = about(TAG, tag.name, "a string tag cannot be retained");
        return INVALID_VALUE;
    }
    if (tag.writable_by_command && tag.type == TagType::STRING) {
        detail =
            about(TAG, tag.name, "a string tag cannot be writable_by_command");
        return INVALID_VALUE;
    }
    std::uint8_t flags = 0;
    if (tag.retain) {
        flags |= to_raw(TagFlag::RETAIN);
    }
    if (tag.writable_by_command) {
        flags |= to_raw(TagFlag::WRITABLE_BY_COMMAND);
    }
    if (tag.retain_now) {
        flags |= to_raw(TagFlag::RETAIN_NOW);
    }
    return flags;
}

// Contract section 8.3: a change of every tag of an owner must fit its ring.
Status check_ring_capacities(const TagSet& tag_set,
                             const ResolvedTagSet& resolved,
                             std::string& detail) {
    for (const ResolvedOwner& owner : resolved.owners) {
        const OwnerDefinition& definition = tag_set.owners.at(owner.definition);
        const std::uint64_t needed = std::uint64_t{2} * owner.tag_count;
        if (definition.change_ring_capacity < needed) {
            detail = about(OWNER, definition.name,
                           "change_ring_capacity " +
                               std::to_string(definition.change_ring_capacity) +
                               " is less than twice its " +
                               std::to_string(owner.tag_count) + " tags");
            return LIMIT_EXCEEDED;
        }
    }
    return {};
}

Status resolve_tags(const TagSet& tag_set, ResolvedTagSet& resolved,
                    std::string& detail) {
    const std::vector<std::size_t> order = order_by_name(tag_set.tags);
    if (const Status status = check_list(tag_set.tags, order, TAG, MAX_TAGS,
                                         MAX_TAG_NAME_LENGTH, detail);
        !status.ok()) {
        return status;
    }
    std::vector<std::size_t> owner_order;
    owner_order.reserve(resolved.owners.size());
    for (const ResolvedOwner& owner : resolved.owners) {
        owner_order.push_back(owner.definition);
    }

    resolved.tags.reserve(order.size());
    for (const std::size_t position : order) {
        const TagDefinition& tag = tag_set.tags[position];
        const Result<std::uint8_t> flags = tag_flags(tag, detail);
        if (!flags.ok()) {
            return flags.status();
        }
        const auto owner_index =
            find_by_name(tag_set.owners, owner_order, tag.owner);
        if (!owner_index.has_value()) {
            detail =
                about(TAG, tag.name,
                      is_valid_name(tag.owner, MAX_SHORT_NAME_LENGTH)
                          ? "owner " + quoted(tag.owner) +
                                " is not an owner of the tag set"
                          : std::string("the owner is not a valid owner name"));
            return UNKNOWN_REFERENCE;
        }
        ResolvedOwner& owner = resolved.owners.at(*owner_index);
        resolved.tags.push_back(
            ResolvedTag{.definition = position,
                        // Below MAX_OWNERS.
                        .owner_index = static_cast<std::uint32_t>(*owner_index),
                        .slot = owner.tag_count,
                        .flags = flags.value()});
        // At most MAX_TAGS tags in all, so neither count can wrap.
        ++owner.tag_count;
        if (tag.type == TagType::STRING) {
            ++owner.string_slots;
        }
    }
    return check_ring_capacities(tag_set, resolved, detail);
}

// Feeds the canonical encoding of a tag set to a hash, one entry at a time:
// integers little-endian, text as a 32-bit length followed by its bytes.
class CanonicalEncoder {
public:
    void put_u8(std::uint8_t value) { entry_.push_back(value); }

    void put_u32(std::uint32_t value) {
        constexpr unsigned BITS_PER_BYTE = 8;
        for (unsigned i = 0; i < sizeof(value); ++i) {
            // Truncation to the low byte is the intent.
            entry_.push_back(
                static_cast<std::uint8_t>(value >> (i * BITS_PER_BYTE)));
        }
    }

    void put_count(std::size_t count) {
        // Every count is bounded by a limit of the contract below 2^32.
        put_u32(static_cast<std::uint32_t>(count));
    }

    void put_text(std::string_view text) {
        put_count(text.size());
        for (const char character : text) {
            entry_.push_back(static_cast<std::uint8_t>(character));
        }
    }

    // Hashes what was put since the previous call.
    void end_entry() {
        hash_.update(entry_);
        entry_.clear();
    }

    [[nodiscard]] Sha256Digest finish() {
        end_entry();
        return hash_.finish();
    }

private:
    Sha256 hash_;
    std::vector<std::uint8_t> entry_;
};

// Names the encoding, so that a later change of it cannot produce the hash
// of a different tag set by accident.
constexpr std::string_view CANONICAL_ENCODING_NAME = "scada-tag-set";
constexpr std::uint32_t CANONICAL_ENCODING_VERSION = 1;

}  // namespace

bool is_valid_name(std::string_view name, std::size_t max_length) noexcept {
    if (name.empty() || name.size() > max_length) {
        return false;
    }
    return std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' ||
               character == '.' || character == '-';
    });
}

std::uint32_t queue_record_size(QueueKind kind) noexcept {
    switch (kind) {
        case QueueKind::EVENTS:
            return sizeof(EventRecord);
        case QueueKind::CMD:
            return sizeof(WriteRequest);
        case QueueKind::RESULT:
            return sizeof(WriteResult);
    }
    return 0;
}

Result<ResolvedTagSet> resolve(const TagSet& tag_set, std::string& detail) {
    if (tag_set.heartbeat_check_ms == 0) {
        detail = "heartbeat_check_ms must be at least 1";
        return INVALID_VALUE;
    }
    ResolvedTagSet resolved;
    if (const Status status = resolve_processes(tag_set, resolved, detail);
        !status.ok()) {
        return status;
    }
    if (const Status status = resolve_owners(tag_set, resolved, detail);
        !status.ok()) {
        return status;
    }
    if (const Status status = resolve_queues(tag_set, resolved, detail);
        !status.ok()) {
        return status;
    }
    if (const Status status = resolve_tags(tag_set, resolved, detail);
        !status.ok()) {
        return status;
    }
    return resolved;
}

ConfigHash config_hash_of(const TagSet& tag_set,
                          const ResolvedTagSet& resolved) {
    CanonicalEncoder encoder;
    encoder.put_text(CANONICAL_ENCODING_NAME);
    encoder.put_u32(CANONICAL_ENCODING_VERSION);
    encoder.put_u32(tag_set.heartbeat_check_ms);

    encoder.put_count(resolved.processes.size());
    for (const std::size_t position : resolved.processes) {
        const ProcessDefinition& process = tag_set.processes.at(position);
        encoder.put_text(process.name);
        encoder.put_u32(process.uid);
        encoder.put_u32(process.heartbeat_timeout_ms);
        encoder.put_u32(process.realtime ? to_raw(ProcessFlag::REALTIME) : 0);
        encoder.end_entry();
    }

    encoder.put_count(resolved.owners.size());
    for (const ResolvedOwner& resolved_owner : resolved.owners) {
        const OwnerDefinition& owner =
            tag_set.owners.at(resolved_owner.definition);
        encoder.put_text(owner.name);
        encoder.put_text(owner.process);
        encoder.put_u32(owner.change_ring_capacity);
        encoder.end_entry();
    }

    encoder.put_count(resolved.queues.size());
    for (const ResolvedQueue& resolved_queue : resolved.queues) {
        const QueueDefinition& queue =
            tag_set.queues.at(resolved_queue.definition);
        encoder.put_text(queue.name);
        encoder.put_text(queue.producer);
        encoder.put_text(queue.consumer);
        encoder.put_u32(queue.capacity);
        encoder.end_entry();
    }

    encoder.put_count(resolved.tags.size());
    for (const ResolvedTag& resolved_tag : resolved.tags) {
        const TagDefinition& tag = tag_set.tags.at(resolved_tag.definition);
        encoder.put_text(tag.name);
        encoder.put_text(tag.owner);
        encoder.put_u8(to_raw(tag.type));
        encoder.put_u8(resolved_tag.flags);
        encoder.end_entry();
    }
    return encoder.finish();
}

Status validate(const TagSet& tag_set, std::string& detail) {
    return resolve(tag_set, detail).status();
}

Result<ConfigHash> config_hash(const TagSet& tag_set, std::string& detail) {
    const Result<ResolvedTagSet> resolved = resolve(tag_set, detail);
    if (!resolved.ok()) {
        return resolved.status();
    }
    return config_hash_of(tag_set, resolved.value());
}

}  // namespace scada::tag
