#ifndef SCADA_SIM_LAYOUT_H
#define SCADA_SIM_LAYOUT_H

// The shape of a simulation (issue P1-10): how many processes, owners and
// tags, what they are called and which pattern each tag follows. The tag-set
// generator and every simulator process compute the same names from the
// same Layout, so they agree without reading a file.
//
// Names, for prefix `p`:
// - process: `p` if there is one process, else `p<n>`, n from 0;
// - owner: the process name if the process has one owner, else
//   `<process>.w<k>`, k from 0 within the process (contract, section 2);
// - tag: `<owner>.t<i>`, i from 0 within the owner.
// Owners are numbered across all processes: owner g is owner
// g % owners_per_process of process g / owners_per_process. The tags are
// shared out as evenly as possible; the first `tags % owners` owners get one
// more. Tag i of every owner follows patterns[i % patterns.size()].

#include "scada/common/status.h"
#include "scada/sim/pattern.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scada::sim {

// At most this many different patterns in one Layout.
inline constexpr std::size_t MAX_PATTERNS = 16;
inline constexpr std::uint64_t DEFAULT_TAGS = 1000;

struct Layout {
    std::string prefix = "sim";
    std::uint32_t processes = 1;
    std::uint32_t owners = 1;           // of all processes together
    std::uint64_t tags = DEFAULT_TAGS;  // of all owners together
    std::vector<Pattern> patterns{Pattern::CHECK};
};

// Checks `layout`: at least one process, owners a multiple of the processes,
// at least one tag per owner, at most 1 to MAX_PATTERNS patterns, and every
// count and every name within the limits of the tag database contract
// (sections 1 and 14). Returns OK or the first problem found; on an error
// `detail` is replaced by a sentence that names it. Every function below
// takes a Layout that passed this check, and indexes within its counts.
[[nodiscard]] Status check_layout(const Layout& layout, std::string& detail);

[[nodiscard]] std::uint32_t owners_per_process(const Layout& layout) noexcept;

// Name of process `process`.
[[nodiscard]] std::string process_name(const Layout& layout,
                                       std::uint32_t process);

// Name of owner `owner`, numbered across all processes.
[[nodiscard]] std::string owner_name(const Layout& layout, std::uint32_t owner);

// The process that owner `owner` is a thread of.
[[nodiscard]] std::uint32_t process_of_owner(const Layout& layout,
                                             std::uint32_t owner) noexcept;

// How many tags owner `owner` has.
[[nodiscard]] std::uint64_t tags_of_owner(const Layout& layout,
                                          std::uint32_t owner) noexcept;

// Name of tag `tag` of owner `owner`.
[[nodiscard]] std::string tag_name(const Layout& layout, std::uint32_t owner,
                                   std::uint64_t tag);

// The pattern of tag `tag` of any owner.
[[nodiscard]] Pattern pattern_of(const Layout& layout,
                                 std::uint64_t tag) noexcept;

}  // namespace scada::sim

#endif  // SCADA_SIM_LAYOUT_H
