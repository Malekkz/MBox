#ifndef SCADA_TAG_HEADER_BUILDER_H
#define SCADA_TAG_HEADER_BUILDER_H

// Builds the image of the `header` region of one generation
// (docs/contracts/tag-database.md, section 5) from a tag-set definition.
// scada-tagd writes the image to the `header` file; this library does no
// file I/O here.

#include "scada/common/result.h"
#include "scada/tag/tag_set.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scada::tag {

// What the header holds besides the tag set.
struct HeaderParameters {
    std::uint64_t generation = 0;
    std::int64_t created_time_ns = 0;  // CLOCK_REALTIME
};

// The bytes of a complete `header` region, and the hash stored in it.
struct HeaderImage {
    // Exactly the content of the file: `RegionHeader.size_bytes` equals
    // `bytes.size()`.
    std::vector<std::byte> bytes;
    ConfigHash config_hash{};
};

// Builds the `header` region for `tag_set`.
//
// The image is not yet usable by readers: `RegionHeader.ready` is 0 and
// `GlobalHeader.state` is BUILDING. scada-tagd sets them after writing the
// file (contract, section 11.1).
//
// Indexes are given in name order: processes, owners and queues are sorted
// by name, and the slots of an owner are its tags sorted by name. The same
// tag set therefore gives the same image whatever the order of its entries.
//
// Fails, with `detail` as for `validate`, if the tag set is not valid.
[[nodiscard]] Result<HeaderImage> build_header(
    const TagSet& tag_set, const HeaderParameters& parameters,
    std::string& detail);

}  // namespace scada::tag

#endif  // SCADA_TAG_HEADER_BUILDER_H
