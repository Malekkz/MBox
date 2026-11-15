#include "scada/sim/layout.h"

#include "scada/common/status.h"
#include "scada/sim/pattern.h"
#include "scada/tag/handle.h"
#include "scada/tag/tag_set.h"

#include <cstdint>
#include <string>

namespace scada::sim {

namespace {

constexpr Status INVALID(StatusCode::INVALID_ARGUMENT,
                         "the simulation layout is not valid");
constexpr Status TOO_LARGE(StatusCode::OUT_OF_RANGE,
                           "the simulation layout exceeds a limit");

}  // namespace

Status check_layout(const Layout& layout, std::string& detail) {
    if (layout.processes == 0 || layout.owners == 0) {
        detail = "a simulation needs at least one process and one owner";
        return INVALID;
    }
    if (layout.owners % layout.processes != 0) {
        detail = "the owners must be a multiple of the processes";
        return INVALID;
    }
    // One process of the tag set is scada-tagd.
    if (layout.processes >= scada::tag::MAX_PROCESSES ||
        layout.owners > scada::tag::MAX_OWNERS) {
        detail = "more processes or owners than the tag database allows";
        return TOO_LARGE;
    }
    if (layout.tags < layout.owners) {
        detail = "every owner needs at least one tag";
        return INVALID;
    }
    if (layout.tags > scada::tag::MAX_TAGS) {
        detail = "more tags than the tag database allows";
        return TOO_LARGE;
    }
    if (layout.patterns.empty() || layout.patterns.size() > MAX_PATTERNS) {
        detail = "a simulation needs 1 to 16 patterns";
        return INVALID;
    }
    if (!scada::tag::is_valid_name(layout.prefix,
                                   scada::tag::MAX_SHORT_NAME_LENGTH)) {
        detail = "the prefix is not a process name";
        return INVALID;
    }
    // The longest names are those with the highest numbers. Owner 0 has the
    // most tags.
    const std::uint32_t last_owner = layout.owners - 1;
    if (!scada::tag::is_valid_name(process_name(layout, layout.processes - 1),
                                   scada::tag::MAX_SHORT_NAME_LENGTH) ||
        !scada::tag::is_valid_name(owner_name(layout, last_owner),
                                   scada::tag::MAX_SHORT_NAME_LENGTH) ||
        !scada::tag::is_valid_name(
            tag_name(layout, last_owner, tags_of_owner(layout, 0) - 1),
            scada::tag::MAX_TAG_NAME_LENGTH)) {
        detail = "the prefix is too long for the names it makes";
        return INVALID;
    }
    return {};
}

std::uint32_t owners_per_process(const Layout& layout) noexcept {
    return layout.owners / layout.processes;
}

std::string process_name(const Layout& layout, std::uint32_t process) {
    if (layout.processes == 1) {
        return layout.prefix;
    }
    return layout.prefix + std::to_string(process);
}

std::string owner_name(const Layout& layout, std::uint32_t owner) {
    const std::uint32_t per_process = owners_per_process(layout);
    std::string name = process_name(layout, owner / per_process);
    if (per_process > 1) {
        name += ".w" + std::to_string(owner % per_process);
    }
    return name;
}

std::uint32_t process_of_owner(const Layout& layout,
                               std::uint32_t owner) noexcept {
    return owner / owners_per_process(layout);
}

std::uint64_t tags_of_owner(const Layout& layout,
                            std::uint32_t owner) noexcept {
    const std::uint64_t share = layout.tags / layout.owners;
    return share + (owner < layout.tags % layout.owners ? 1 : 0);
}

std::string tag_name(const Layout& layout, std::uint32_t owner,
                     std::uint64_t tag) {
    return owner_name(layout, owner) + ".t" + std::to_string(tag);
}

Pattern pattern_of(const Layout& layout, std::uint64_t tag) noexcept {
    return layout.patterns[tag % layout.patterns.size()];
}

}  // namespace scada::sim
