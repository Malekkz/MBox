#ifndef SCADA_CONF_SRC_TAG_NAMES_H
#define SCADA_CONF_SRC_TAG_NAMES_H

// The tags of the expanded tree by name, with their value types
// (configuration schemas contract 12.4, 12.5; decision D-116), for the
// reference checks of stage 5. Names and types only: the tag set with its
// owners, queues and hash is #101, after the owner-name rule of #151, so
// nothing here names an owner.

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace scada::conf {

// Where a tag comes from.
enum class TagOrigin : std::uint8_t {
    POINT,      // `<device>.<point>`
    BIT,        // `<device>.<bit>`
    TAG_FILE,   // `tags/*.yaml`: internal, manual or virtual
    INTERLOCK,  // an interlock's name
    GENERATED,  // section 12.5
};

// One tag: its value type as section 4.7 names them (`bool`, `int64`,
// `uint64`, `float64`, `string`, ...), empty while it is not known (a
// virtual tag, typed from its expression with slice 7b); where it comes
// from; the place it was defined.
struct TagInfo {
    std::string value_type;
    TagOrigin origin = TagOrigin::GENERATED;
    Location where;
    // A point or bit tag: its point in the expanded tree. Zero otherwise.
    NodeId point = 0;
    // A point or bit tag: the protocol of its channel.
    std::string protocol;
};

// The tags of one tree, by name.
class TagNames {
public:
    // The tag of this name, or nothing.
    [[nodiscard]] const TagInfo* find(std::string_view name) const;

    [[nodiscard]] std::size_t size() const noexcept { return tags_.size(); }

    // Adds `name`; the tag already of that name if there is one, which is
    // kept.
    const TagInfo* add(std::string name, TagInfo info);

private:
    std::map<std::string, TagInfo, std::less<>> tags_;
};

// The tags of `tree` and `files` (12.4, 12.5): point and bit tags, the tags
// of tag files, interlocks, and the generated tags of outputs, areas,
// alarms, processes, channels and devices. Adds CF-301 for a tag that a
// user writes in `sys`, and CF-302 for a name given twice, generated names
// included. Tunable parameters of blocks (`ctl.*`) are not built: the block
// library is not in phase 2.
[[nodiscard]] TagNames collect_tags(const ExpandedTree& tree,
                                    std::span<const CheckedFile> files,
                                    Diagnostics& diagnostics);

// Checks every reference to a tag outside expressions, and every reference
// to a point or a poll group within a device:
// - CF-310 for an output's `target` or `feedback`, an area's
//   `local_panel.input`, an alarm's `tag`, `limit` (a tag) or `setpoint`, a
//   program's source tag, that names no tag of `tags`;
// - CF-312 for an alarm's `limit` or `setpoint` tag that is not a number;
// - CF-310 for `exponent_from`, `quality_from`, `source_time_from`, a health
//   rule, a restart indication or an identity check naming no point of the
//   device;
// - CF-604 for a point's `poll_group` that its channel does not have.
// The references within run-time expressions are slice 7b's.
void check_tag_references(const TagNames& tags, const ExpandedTree& tree,
                          std::span<const CheckedFile> files,
                          Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_TAG_NAMES_H
