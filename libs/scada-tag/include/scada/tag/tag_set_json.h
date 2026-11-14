#ifndef SCADA_TAG_TAG_SET_JSON_H
#define SCADA_TAG_TAG_SET_JSON_H

// The temporary tag-set file format of phase 1: one JSON document that lists
// processes, owners, queues and tags. The real configuration replaces it in
// phase 2; nothing but this header knows the format. `README.md` of this
// library describes it.

#include "scada/common/result.h"
#include "scada/tag/tag_set.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace scada::tag {

// The version this parser reads (`"format"` in the file).
inline constexpr std::uint32_t TAG_SET_JSON_FORMAT = 1;

// The largest document the parser accepts, in bytes. A tag set of
// MAX_TAGS tags with names of average length fits.
inline constexpr std::size_t MAX_TAG_SET_JSON_BYTES =
    std::size_t{256} * 1024 * 1024;

// Parses a tag-set file. `text` is outside input: the whole document is
// parsed and checked before anything is returned, and nothing in it drives
// an allocation or a loop before it is checked against the limits of the
// contract.
//
// The result holds the entries in file order, with defaults filled in. It
// has the right shape but is not yet checked against the contract; `validate`,
// `config_hash` and `build_header` do that.
//
// On an error `detail` is replaced by "line <l>, column <c>: <what>";
// otherwise it is left unchanged.
[[nodiscard]] Result<TagSet> parse_tag_set_json(std::string_view text,
                                                std::string& detail);

}  // namespace scada::tag

#endif  // SCADA_TAG_TAG_SET_JSON_H
