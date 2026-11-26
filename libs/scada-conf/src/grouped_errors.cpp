// One cause, one error (D-114, D-115).

#include "grouped_errors.h"

#include "scada/conf/diagnostic.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace scada::conf {

namespace {

// The others of a cause named in its message (S7 of the delta review of
// #146).
constexpr std::size_t NAMED_INSTANCES = 10;

}  // namespace

void GroupedErrors::add(Diagnostic diagnostic, std::string_view key,
                        std::string_view instance, Diagnostics& diagnostics) {
    // A cause is looked up before any text is kept, so that one met again
    // keeps nothing (D2 of the delta review of #146).
    std::string cause;
    if (!diagnostic.notes.empty() &&
        diagnostic.notes.front().kind == "template") {
        const Location& site = diagnostic.notes.front().location;
        cause = std::to_string(diagnostic.code);
        cause += '|';
        cause += site.file;
        cause += ':' + std::to_string(site.line) + ':' +
                 std::to_string(site.column) + '|';
        cause += key;
        if (const auto group = causes_.find(cause); group != causes_.end()) {
            // Counted once per instance: points of one instance that meet
            // the cause are one.
            Cause& seen = group->second;
            const Location& place = diagnostic.location;
            if (place.file != seen.last.file || place.line != seen.last.line ||
                place.column != seen.last.column) {
                ++seen.more;
                seen.last = place;
                if (seen.names.size() < NAMED_INSTANCES) {
                    seen.names.emplace_back(instance);
                }
            }
            return;
        }
    }
    if (pending_.size() >= kept_limit_) {
        diagnostics.count_unlisted_errors(1);
        return;
    }
    if (!reported_.insert(format(diagnostic)).second) {
        return;
    }
    if (!cause.empty()) {
        causes_.emplace(std::move(cause), Cause{.first = pending_.size(),
                                                .more = 0,
                                                .last = diagnostic.location,
                                                .names = {}});
    }
    pending_.push_back(std::move(diagnostic));
}

void GroupedErrors::flush(Diagnostics& diagnostics) {
    for (const auto& [cause, group] : causes_) {
        if (group.more == 0) {
            continue;
        }
        std::string others =
            " (and " + std::to_string(group.more) +
            (group.more == 1 ? " more instance: " : " more instances: ");
        for (std::size_t i = 0; i < group.names.size(); ++i) {
            others += (i == 0 ? "" : ", ") + group.names[i];
        }
        others += group.more > group.names.size() ? ", ...)" : ")";
        pending_[group.first].message += others;
    }
    for (Diagnostic& diagnostic : pending_) {
        diagnostics.add(std::move(diagnostic));
    }
    pending_.clear();
    causes_.clear();
}

}  // namespace scada::conf
