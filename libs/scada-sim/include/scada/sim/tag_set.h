#ifndef SCADA_SIM_TAG_SET_H
#define SCADA_SIM_TAG_SET_H

// The tag set of a simulation: what `scada-tagd` must be given so that the
// simulator processes of a Layout can attach and write, for example "50,000
// tags across 10 owners" (issue P1-10).

#include "scada/common/result.h"
#include "scada/sim/layout.h"
#include "scada/tag/tag_set.h"

#include <cstdint>
#include <string>
#include <vector>

namespace scada::sim {

struct TagSetOptions {
    Layout layout;
    std::uint32_t uid = 0;       // user of the simulator processes and readers
    std::uint32_t tagd_uid = 0;  // user of scada-tagd
    // Further processes without tags, for example readers of a test.
    std::vector<std::string> readers;
    std::uint32_t heartbeat_check_ms = scada::tag::DEFAULT_HEARTBEAT_CHECK_MS;
    // Of the simulator processes and the readers.
    std::uint32_t heartbeat_timeout_ms =
        scada::tag::DEFAULT_HEARTBEAT_TIMEOUT_MS;
    // Of every owner. 0: the default of the contract, 65,536, or the next
    // power of two of twice the owner's tags if that is more.
    std::uint32_t ring_capacity = 0;
};

// The tag set: the process `tagd`, the processes, owners and tags of the
// layout, and the readers. It holds no queues. Fails with `detail` filled,
// as `check_layout` and `scada::tag::validate` do, if the layout or the
// resulting tag set is not valid.
[[nodiscard]] Result<scada::tag::TagSet> make_tag_set(
    const TagSetOptions& options, std::string& detail);

// `tag_set` in the temporary tag-set file format that scada-tagd reads
// (`scada/tag/tag_set_json.h`), every field written out. Precondition:
// `tag_set` passed `scada::tag::validate`, so that no name needs escaping.
[[nodiscard]] std::string tag_set_json(const scada::tag::TagSet& tag_set);

}  // namespace scada::sim

#endif  // SCADA_SIM_TAG_SET_H
