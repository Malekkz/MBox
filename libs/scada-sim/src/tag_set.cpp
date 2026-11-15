#include "scada/sim/tag_set.h"

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/sim/layout.h"
#include "scada/sim/pattern.h"
#include "scada/tag/tag_set.h"
#include "scada/tag/tag_set_json.h"
#include "scada/tag/types.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scada::sim {

namespace {

using scada::tag::OwnerDefinition;
using scada::tag::ProcessDefinition;
using scada::tag::TagDefinition;
using scada::tag::TagSet;
using scada::tag::TagType;

// The ring capacity of an owner with `tags` tags when none is given: the
// default of the contract, or the next power of two of twice the tags if
// that is more. A capacity beyond the contract's limit is left for
// `validate` to refuse.
std::uint32_t default_ring_capacity(std::uint64_t tags) {
    const std::uint64_t needed = std::bit_ceil(2 * tags);
    if (needed <= scada::tag::DEFAULT_CHANGE_RING_CAPACITY) {
        return scada::tag::DEFAULT_CHANGE_RING_CAPACITY;
    }
    // Tags are at most MAX_TAGS, so `needed` is at most 2^21.
    return static_cast<std::uint32_t>(needed);
}

std::string_view type_name(TagType type) {
    switch (type) {
        case TagType::BOOL:
            return "bool";
        case TagType::INT64:
            return "int64";
        case TagType::UINT64:
            return "uint64";
        case TagType::FLOAT64:
            return "float64";
        case TagType::STRING:
            return "string";
    }
    return "";
}

// The pieces of the JSON text. Names and keys need no escaping: they
// passed the name grammar of the contract.
constexpr std::string_view LIST_START = "\n  ";
constexpr std::string_view LIST_NEXT = ",\n  ";
constexpr std::string_view LIST_END = "]";

// An object being written: whether a key was written yet, and what
// separates two keys.
struct Object {
    bool first = true;
    std::string_view separator = ", ";
};

// Appends the key `name` of `object`: the opening brace before its first
// key, else the separator.
void add_key(std::string& text, std::string_view name, Object& object) {
    text += object.first ? std::string_view("{") : object.separator;
    object.first = false;
    text += '"';
    text += name;
    text += R"(": )";
}

void add_text(std::string& text, std::string_view name, std::string_view value,
              Object& object) {
    add_key(text, name, object);
    text += '"';
    text += value;
    text += '"';
}

void add_number(std::string& text, std::string_view name, std::uint32_t value,
                Object& object) {
    add_key(text, name, object);
    text += std::to_string(value);
}

void add_flag(std::string& text, std::string_view name, bool value,
              Object& object) {
    add_key(text, name, object);
    text += value ? "true" : "false";
}

}  // namespace

Result<TagSet> make_tag_set(const TagSetOptions& options, std::string& detail) {
    const Layout& layout = options.layout;
    if (const Status checked = check_layout(layout, detail); !checked.ok()) {
        return checked;
    }
    TagSet tag_set;
    tag_set.heartbeat_check_ms = options.heartbeat_check_ms;
    tag_set.processes.push_back(
        ProcessDefinition{.name = "tagd",
                          .uid = options.tagd_uid,
                          .heartbeat_timeout_ms = options.heartbeat_timeout_ms,
                          .realtime = false});
    for (std::uint32_t process = 0; process < layout.processes; ++process) {
        tag_set.processes.push_back(ProcessDefinition{
            .name = process_name(layout, process),
            .uid = options.uid,
            .heartbeat_timeout_ms = options.heartbeat_timeout_ms,
            .realtime = false});
    }
    for (const std::string& reader : options.readers) {
        tag_set.processes.push_back(ProcessDefinition{
            .name = reader,
            .uid = options.uid,
            .heartbeat_timeout_ms = options.heartbeat_timeout_ms,
            .realtime = false});
    }
    tag_set.owners.reserve(layout.owners);
    // In range: tags is at most MAX_TAGS.
    tag_set.tags.reserve(static_cast<std::size_t>(layout.tags));
    for (std::uint32_t owner = 0; owner < layout.owners; ++owner) {
        const std::uint64_t tags = tags_of_owner(layout, owner);
        const std::string owner_text = owner_name(layout, owner);
        tag_set.owners.push_back(OwnerDefinition{
            .name = owner_text,
            .process = process_name(layout, process_of_owner(layout, owner)),
            .change_ring_capacity = options.ring_capacity != 0
                                        ? options.ring_capacity
                                        : default_ring_capacity(tags)});
        for (std::uint64_t tag = 0; tag < tags; ++tag) {
            tag_set.tags.push_back(
                TagDefinition{.name = owner_text + ".t" + std::to_string(tag),
                              .owner = owner_text,
                              .type = tag_type(pattern_of(layout, tag)),
                              .retain = false,
                              .writable_by_command = false,
                              .retain_now = false});
        }
    }
    if (const Status valid = scada::tag::validate(tag_set, detail);
        !valid.ok()) {
        return valid;
    }
    return tag_set;
}

std::string tag_set_json(const TagSet& tag_set) {
    std::string text;
    Object first{.first = true, .separator = ",\n "};
    add_number(text, "format", scada::tag::TAG_SET_JSON_FORMAT, first);
    add_number(text, "heartbeat_check_ms", tag_set.heartbeat_check_ms, first);
    add_key(text, "processes", first);
    text += '[';
    std::string_view separator = LIST_START;
    for (const ProcessDefinition& process : tag_set.processes) {
        text += separator;
        Object entry;
        add_text(text, "name", process.name, entry);
        add_number(text, "uid", process.uid, entry);
        add_number(text, "heartbeat_timeout_ms", process.heartbeat_timeout_ms,
                   entry);
        add_flag(text, "realtime", process.realtime, entry);
        text += '}';
        separator = LIST_NEXT;
    }
    text += LIST_END;
    add_key(text, "owners", first);
    text += '[';
    separator = LIST_START;
    for (const OwnerDefinition& owner : tag_set.owners) {
        text += separator;
        Object entry;
        add_text(text, "name", owner.name, entry);
        add_text(text, "process", owner.process, entry);
        add_number(text, "change_ring_capacity", owner.change_ring_capacity,
                   entry);
        text += '}';
        separator = LIST_NEXT;
    }
    text += LIST_END;
    add_key(text, "queues", first);
    text += '[';
    separator = LIST_START;
    for (const scada::tag::QueueDefinition& queue : tag_set.queues) {
        text += separator;
        Object entry;
        add_text(text, "name", queue.name, entry);
        add_text(text, "producer", queue.producer, entry);
        add_text(text, "consumer", queue.consumer, entry);
        add_number(text, "capacity", queue.capacity, entry);
        text += '}';
        separator = LIST_NEXT;
    }
    text += LIST_END;
    add_key(text, "tags", first);
    text += '[';
    separator = LIST_START;
    for (const TagDefinition& tag : tag_set.tags) {
        text += separator;
        Object entry;
        add_text(text, "name", tag.name, entry);
        add_text(text, "owner", tag.owner, entry);
        add_text(text, "type", type_name(tag.type), entry);
        add_flag(text, "retain", tag.retain, entry);
        add_flag(text, "writable_by_command", tag.writable_by_command, entry);
        add_flag(text, "retain_now", tag.retain_now, entry);
        text += '}';
        separator = LIST_NEXT;
    }
    text += LIST_END;
    text += "}\n";
    return text;
}

}  // namespace scada::sim
