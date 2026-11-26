#ifndef SCADA_CONF_SRC_TAG_NAMES_H
#define SCADA_CONF_SRC_TAG_NAMES_H

// The tags of the expanded tree by name, with their value types
// (configuration schemas contract 12.4, 12.5; decision D-116), for the
// checks of stage 5 and for the tag set of stage 6 (#101), which takes them
// from the same call. Names and types only: owners, queues and the hash are
// #101's, after the owner-name rule of #151, so nothing here names an owner.

#include "scada/conf/diagnostic.h"
#include "scada/conf/expand.h"
#include "scada/conf/node.h"
#include "scada/conf/references.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::conf {

// Where a tag comes from.
enum class TagOrigin : std::uint8_t {
    POINT,      // `<device>.<point>`
    BIT,        // `<device>.<bit>`
    TAG_FILE,   // `tags/*.yaml`: internal, manual or virtual
    INTERLOCK,  // an interlock's name
    // The state tags of an alarm on a tag (`<tag>.alarm.<id>` and its
    // `shelved_until`): two of one name are CF-812's (slice 7c). An
    // expression alarm is on no tag: its state tags are GENERATED.
    ALARM_STATE,
    GENERATED,  // any other tag of section 12.5
};

// One tag. Its texts are views into the Documents of the tree, or constant:
// a TagNames does not outlive the call of `validate_and_expand` that made
// it (D-116).
struct TagInfo {
    // The value type as section 4.7 names it (`bool`, `int64`, `uint64`,
    // `float64`, `string`); empty for a virtual tag not typed yet (D-117).
    std::string_view value_type;
    TagOrigin origin = TagOrigin::GENERATED;
    // Where it is defined, or made from: a node of `document`, which is the
    // expanded tree for a point or a bit, a device's or channel's tag and the
    // state tags of their alarms; nothing for a process's tag.
    const Document* document = nullptr;
    NodeId node = 0;
    // A point or bit tag: its point in the expanded tree, its device and the
    // protocol of its channel. Zero and empty otherwise.
    NodeId point = 0;
    std::string_view device;
    std::string_view protocol;
};

class TagCollector;

// The tags of one tree, by name. The tags of a device (`sys.dev.*`) and of
// a channel (`sys.drv.*`) are not stored one by one: they are found from
// the device and channel names and the signal tables of driver interface
// contract 11.1 (D-116), so that memory grows with the tags a user wrote.
class TagNames {
public:
    // The tag of this name, or nothing.
    [[nodiscard]] std::optional<TagInfo> find(std::string_view name) const;

    // The number of tags, generated tags included (CF-1001).
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    // False when the tags passed `limits.max_tags` (CF-1001): the set is
    // not whole, and checks that look tags up are not made.
    [[nodiscard]] bool complete() const noexcept { return complete_; }

    // The points of device `device` of the expanded tree, sorted, with and
    // without a tag of their own; nothing for a name that is no device, or
    // a device given twice after its first (CF-303).
    [[nodiscard]] const std::vector<std::string_view>* points_of(
        std::string_view device) const;

    // The poll groups of channel `channel` (4.6): its own, which replace the
    // defaults `fast`, `normal`, `slow` (D-116); nothing for a name that is
    // no channel.
    [[nodiscard]] const std::vector<std::string_view>* poll_groups_of(
        std::string_view channel) const;

    // The `name` node in the expanded tree of the first device of this
    // name, 0 for none: a device given twice is checked once (CF-303).
    [[nodiscard]] NodeId device_at(std::string_view device) const;

    // Gives tag `name` its value type: a virtual tag's, from its expression
    // (D-117). Nothing for a name that is no tag.
    void set_value_type(std::string_view name, std::string value_type);

    // Calls `visit(name, info)` for each tag whose name starts with
    // `prefix`, in the order of names.
    void each_with_prefix(
        std::string_view prefix,
        const std::function<void(std::string_view, const TagInfo&)>& visit)
        const;

private:
    friend class TagCollector;

    struct Device {
        NodeId name = 0;
        bool identify = false;
        // An output with `refresh` targets one of its tags (8.12).
        bool refreshed = false;
        std::vector<std::string_view> points;
    };
    struct Channel {
        NodeId name = 0;
        std::vector<std::string_view> groups;
    };

    [[nodiscard]] std::optional<TagInfo> device_tag(
        std::string_view name) const;
    [[nodiscard]] std::optional<TagInfo> channel_tag(
        std::string_view name) const;

    const Document* expanded_ = nullptr;
    std::map<std::string, TagInfo, std::less<>> tags_;
    std::map<std::string, Device, std::less<>> devices_;
    std::map<std::string, Channel, std::less<>> channels_;
    std::size_t count_ = 0;
    bool complete_ = true;
};

// The tags of `tree` and `files` (12.4, 12.5): point and bit tags, the tags
// of tag files, interlocks, and the generated tags of devices, channels,
// processes, areas, alarms and outputs. Adds to `diagnostics`:
// - CF-301 for a name a user writes in `sys` (a tag, a device, an output,
//   an expression alarm), once, at the name;
// - CF-302 for a name given twice, generated names included, at the second
//   (a generated tag at what made it), but not for the state tags of two
//   alarms of one id on one tag (CF-812, slice 7c), and not for the tags
//   of a device or an output given twice (CF-303);
// - CF-403 for a generated name over 127 bytes;
// - CF-1001 when the tags pass `limits.max_tags`; the rest is not
//   collected.
// Errors on the expanded tree are at the instance with the template note,
// and one cause at many instances is one error (D-114).
// Tunable parameters of blocks (`ctl.*`) are not built: the block library
// is not in phase 2.
[[nodiscard]] TagNames collect_tags(const ExpandedTree& tree,
                                    std::span<const CheckedFile> files,
                                    Diagnostics& diagnostics);

// Checks every reference to a tag outside expressions, and every reference
// to a point or a poll group within a device:
// - CF-310 for an output's `target` or `feedback`, an area's
//   `local_panel.input`, an alarm's `tag` (wherever the alarm stands),
//   `limit` (a tag) or `setpoint`, a program's source tag (`{ tag }` or the
//   tag itself) and the tags its `out` writes, that names no tag of `tags`;
// - CF-312 for an alarm's `limit` or `setpoint` tag that is not a number;
// - CF-310 for `exponent_from`, `quality_from`, `source_time_from`, a health
//   rule's `point` and `applies_to` names, a restart indication or an
//   identity check naming no point of the device, and CF-311 for an
//   `applies_to` pattern that matches none;
// - CF-604 for a point's poll group, given or the default `normal`, that its
//   channel does not have.
// Nothing when `tags` is not complete. The references within run-time
// expressions are slice 7b's.
void check_tag_references(const TagNames& tags, const ExpandedTree& tree,
                          std::span<const CheckedFile> files,
                          Diagnostics& diagnostics);

}  // namespace scada::conf

#endif  // SCADA_CONF_SRC_TAG_NAMES_H
