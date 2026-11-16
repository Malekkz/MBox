#ifndef SCADA_TAG_TAG_SET_H
#define SCADA_TAG_TAG_SET_H

// The tag-set definition: the processes, owners, queues and tags of one
// generation of the tag database (docs/contracts/tag-database.md, sections 2,
// 5 and 14).
//
// `TagSet` is the one interface between configuration and the data plane.
// Whatever produces it (the temporary JSON file of `tag_set_json.h` now, the
// real configuration from phase 2) is replaceable without touching the
// header builder.
//
// The order of the entries in a TagSet carries no meaning: `config_hash` and
// `build_header` give the same result for any order. Where they sort by
// name, they compare the bytes of the names.

#include "scada/common/result.h"
#include "scada/tag/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scada::tag {

// Limits of layout version 1 (contract, sections 1 and 14). MAX_OWNERS and
// MAX_TAGS_PER_OWNER are in handle.h.
inline constexpr std::size_t MAX_TAGS = 1'000'000;
inline constexpr std::size_t MAX_PROCESSES = 1024;
inline constexpr std::size_t MAX_QUEUES = 4096;
inline constexpr std::size_t MAX_TAG_NAME_LENGTH = 127;
// Owners, processes and queues.
inline constexpr std::size_t MAX_SHORT_NAME_LENGTH = 31;
inline constexpr std::uint32_t MIN_CHANGE_RING_CAPACITY = 1024;
inline constexpr std::uint32_t MAX_CHANGE_RING_CAPACITY = 1'048'576;
// Not a limit of the contract: the largest power of two a 32-bit capacity
// field holds.
inline constexpr std::uint32_t MAX_QUEUE_CAPACITY = 0x8000'0000U;
// The largest user id; the one above it is the "no user" value of the
// operating system.
inline constexpr std::uint32_t MAX_UID = 0xFFFF'FFFEU;

// Defaults (contract, sections 10.1 and 14). The queue capacity default is
// this library's choice.
inline constexpr std::uint32_t DEFAULT_HEARTBEAT_CHECK_MS = 250;
// The largest heartbeat_check_ms of a tag set with a retain_now tag
// (contract 12, decision D-62): a RETAIN_NOW change is on disk within 1 s
// when two checks plus a write of 200 ms fit in it, (1000 - 200) / 2.
inline constexpr std::uint32_t MAX_RETAIN_NOW_CHECK_MS = 400;
inline constexpr std::uint32_t DEFAULT_HEARTBEAT_TIMEOUT_MS = 3000;
inline constexpr std::uint32_t DEFAULT_REALTIME_HEARTBEAT_TIMEOUT_MS = 500;
inline constexpr std::uint32_t DEFAULT_CHANGE_RING_CAPACITY = 65'536;
inline constexpr std::uint32_t DEFAULT_QUEUE_CAPACITY = 1024;

// One service, running as its own user.
struct ProcessDefinition {
    std::string name;
    std::uint32_t uid = 0;  // user id the process runs as
    // Whoever fills this in applies the default that fits `realtime`:
    // DEFAULT_REALTIME_HEARTBEAT_TIMEOUT_MS for a real-time process.
    std::uint32_t heartbeat_timeout_ms = DEFAULT_HEARTBEAT_TIMEOUT_MS;
    bool realtime = false;
};

// One writing thread of a process. A process with one writing thread has
// one owner, named like the process; otherwise its owners are named
// `<process>.<writer>`.
struct OwnerDefinition {
    std::string name;
    std::string process;  // name of a process in the same tag set
    // Entries; a power of two, and at least twice the owner's tag count.
    std::uint32_t change_ring_capacity = DEFAULT_CHANGE_RING_CAPACITY;
};

// One reliable queue. The name starts with `events.`, `cmd.` or `result.`,
// which gives the kind of the queue and with it the size of its records.
struct QueueDefinition {
    std::string name;
    std::string producer;  // name of the process that posts
    std::string consumer;  // name of the process that reads
    std::uint32_t capacity = DEFAULT_QUEUE_CAPACITY;  // records, power of two
};

// One tag.
struct TagDefinition {
    std::string name;
    std::string owner;  // name of an owner in the same tag set
    TagType type = TagType::BOOL;
    bool retain = false;
    bool writable_by_command = false;  // not allowed for a STRING tag
    bool retain_now = false;           // needs `retain`
};

// A complete tag-set definition.
struct TagSet {
    // How often scada-tagd checks heartbeats.
    std::uint32_t heartbeat_check_ms = DEFAULT_HEARTBEAT_CHECK_MS;
    std::vector<ProcessDefinition> processes;
    std::vector<OwnerDefinition> owners;
    std::vector<QueueDefinition> queues;
    std::vector<TagDefinition> tags;
};

// SHA-256 of a tag-set definition (`GlobalHeader.config_hash`).
inline constexpr std::size_t CONFIG_HASH_BYTES = 32;
using ConfigHash = std::array<std::uint8_t, CONFIG_HASH_BYTES>;

// True if `name` is a name of the contract (section 1): 1 to `max_length`
// bytes of `[a-z0-9_.-]`.
[[nodiscard]] bool is_valid_name(std::string_view name,
                                 std::size_t max_length) noexcept;

// Checks `tag_set` against the contract: name grammar and lengths, no
// duplicate name within processes, owners, queues or tags, every reference
// to a process or owner resolves, owner naming, queue name prefixes, a
// producer and a consumer that are two processes for every queue, flag
// combinations, and the limits of section 14. Returns OK or the first
// problem found. On an error `detail` is replaced by a sentence that names
// the offending entry, for a log line or a message to the user; otherwise it
// is left unchanged.
[[nodiscard]] Status validate(const TagSet& tag_set, std::string& detail);

// Returns the config hash of `tag_set`: SHA-256 over a canonical encoding of
// the definition with every list sorted by name, so the order of the entries
// does not change it. Fails, with `detail` as for `validate`, if the tag set
// is not valid.
[[nodiscard]] Result<ConfigHash> config_hash(const TagSet& tag_set,
                                             std::string& detail);

}  // namespace scada::tag

#endif  // SCADA_TAG_TAG_SET_H
