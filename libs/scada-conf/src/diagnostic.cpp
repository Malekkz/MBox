#include "scada/conf/diagnostic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scada::conf {

std::string code_text(const Diagnostic& diagnostic) {
    std::string text = diagnostic.severity == Severity::ERROR ? "CF-" : "CW-";
    text += std::to_string(diagnostic.code);
    return text;
}

namespace {

void append_location(std::string& out, const Location& location) {
    out += location.file;
    out += ':';
    out += std::to_string(location.line);
    out += ':';
    out += std::to_string(location.column);
}

}  // namespace

std::string format(const Diagnostic& diagnostic) {
    std::string out;
    append_location(out, diagnostic.location);
    out += diagnostic.severity == Severity::ERROR ? ": error " : ": warning ";
    out += code_text(diagnostic);
    out += ": ";
    out += diagnostic.message;
    for (const Diagnostic::Note& note : diagnostic.notes) {
        out += "\n  from ";
        out += note.kind;
        out += ' ';
        append_location(out, note.location);
        if (!note.detail.empty()) {
            out += " (";
            out += note.detail;
            out += ')';
        }
    }
    return out;
}

bool reported_before(const Diagnostic& lhs, const Diagnostic& rhs) noexcept {
    // std::string compares as unsigned char, so this is byte order.
    const int by_file = lhs.location.file.compare(rhs.location.file);
    if (by_file != 0) {
        return by_file < 0;
    }
    if (lhs.location.line != rhs.location.line) {
        return lhs.location.line < rhs.location.line;
    }
    if (lhs.location.column != rhs.location.column) {
        return lhs.location.column < rhs.location.column;
    }
    return lhs.code < rhs.code;
}

void Diagnostics::error(std::uint16_t code, Location location,
                        std::string message) {
    add(Diagnostic{.severity = Severity::ERROR,
                   .code = code,
                   .location = std::move(location),
                   .message = std::move(message),
                   .notes = {}});
}

void Diagnostics::warning(std::uint16_t code, Location location,
                          std::string message) {
    add(Diagnostic{.severity = Severity::WARNING,
                   .code = code,
                   .location = std::move(location),
                   .message = std::move(message),
                   .notes = {}});
}

void Diagnostics::add(Diagnostic diagnostic) {
    if (diagnostic.severity == Severity::ERROR) {
        ++error_count_;
        errors_.push_back(std::move(diagnostic));
        trim(errors_, false);
    } else {
        ++warning_count_;
        warnings_.push_back(std::move(diagnostic));
        trim(warnings_, false);
    }
}

void Diagnostics::trim(std::vector<Diagnostic>& list, bool force) {
    if (!force && list.size() < 2 * REPORTED_LIMIT) {
        return;
    }
    // A stable sort, so that equal keys keep the order they were added in
    // and the kept set is the same on every run.
    std::stable_sort(list.begin(), list.end(), reported_before);
    if (list.size() > REPORTED_LIMIT) {
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(REPORTED_LIMIT),
                   list.end());
    }
}

namespace {

void append_capped(std::vector<Diagnostic>& into,
                   std::vector<Diagnostic>& from) {
    for (Diagnostic& diagnostic : from) {
        if (into.size() >= Diagnostics::REPORTED_LIMIT) {
            break;
        }
        into.push_back(std::move(diagnostic));
    }
    from.clear();
}

}  // namespace

void Diagnostics::move_into(Report& report) {
    trim(errors_, true);
    trim(warnings_, true);
    append_capped(report.errors, errors_);
    append_capped(report.warnings, warnings_);
    report.error_count += error_count_;
    report.warning_count += warning_count_;
    error_count_ = 0;
    warning_count_ = 0;
}

}  // namespace scada::conf
