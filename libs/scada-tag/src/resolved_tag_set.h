#ifndef SCADA_TAG_RESOLVED_TAG_SET_H
#define SCADA_TAG_RESOLVED_TAG_SET_H

// A tag set after validation, with every name reference turned into an
// index and every index assigned in name order. Both the config hash and the
// header builder start from it, so they cannot disagree about what a tag set
// means.

#include "layout.h"

#include "scada/common/result.h"
#include "scada/tag/tag_set.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scada::tag {

// `definition` in each structure is the position of the entry in the
// corresponding list of the TagSet that was resolved.

struct ResolvedOwner {
    std::size_t definition = 0;
    std::uint16_t process_index = 0;
    std::uint32_t tag_count = 0;
    std::uint32_t string_slots = 0;  // number of STRING tags
};

struct ResolvedQueue {
    std::size_t definition = 0;
    QueueKind kind = QueueKind::EVENTS;
    std::uint16_t producer_process = 0;
    std::uint16_t consumer_process = 0;
};

struct ResolvedTag {
    std::size_t definition = 0;
    std::uint32_t owner_index = 0;
    std::uint32_t slot = 0;  // position among the owner's tags in name order
    std::uint8_t flags = 0;  // TagFlag bits
};

struct ResolvedTagSet {
    // Each list is in name order. The position of a process is its process
    // index, of an owner its owner index, of a queue its queue id.
    std::vector<std::size_t> processes;
    std::vector<ResolvedOwner> owners;
    std::vector<ResolvedQueue> queues;
    std::vector<ResolvedTag> tags;
};

// Validates `tag_set` (see `validate` in tag_set.h) and resolves it. The
// result refers to `tag_set` by position and is meaningful only together
// with it. On an error `detail` names the offending entry.
[[nodiscard]] Result<ResolvedTagSet> resolve(const TagSet& tag_set,
                                             std::string& detail);

// Returns the config hash of a resolved tag set. `resolved` must be the
// result of `resolve(tag_set, ...)`.
[[nodiscard]] ConfigHash config_hash_of(const TagSet& tag_set,
                                        const ResolvedTagSet& resolved);

// Size in bytes of one record of a queue of kind `kind`.
[[nodiscard]] std::uint32_t queue_record_size(QueueKind kind) noexcept;

}  // namespace scada::tag

#endif  // SCADA_TAG_RESOLVED_TAG_SET_H
