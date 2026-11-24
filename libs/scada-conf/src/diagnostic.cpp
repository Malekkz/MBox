#include "scada/conf/diagnostic.h"

#include "utf8.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scada::conf {

namespace {

constexpr unsigned char FIRST_PRINTABLE = 0x20;
constexpr unsigned char ASCII_DEL = 0x7F;
constexpr unsigned char FIRST_NON_ASCII = 0x80;
constexpr std::uint32_t LAST_C1 = 0x9F;
constexpr std::uint32_t LAST_BMP = 0xFFFF;
constexpr std::uint32_t PLANE_LOW_BITS = 0xFFFE;
constexpr unsigned BYTE_DIGITS = 2;
constexpr unsigned BMP_DIGITS = 4;
constexpr unsigned FULL_DIGITS = 8;
constexpr std::string_view ELLIPSIS = "...";
constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";
constexpr unsigned NIBBLE_BITS = 4;
constexpr std::uint32_t NIBBLE_MASK = 0xF;

// A range of code points, both ends included.
struct CodeRange {
    std::uint32_t first;
    std::uint32_t last;
};

// Characters that are invisible, reorder text or are not characters, beyond
// the C0 and C1 controls (D-110).
constexpr std::array<CodeRange, 11> HIDDEN = {{
    {.first = 0x00AD, .last = 0x00AD},    // soft hyphen
    {.first = 0x061C, .last = 0x061C},    // Arabic letter mark
    {.first = 0x180E, .last = 0x180E},    // Mongolian vowel separator
    {.first = 0x200B, .last = 0x200F},    // zero-width characters, marks
    {.first = 0x2028, .last = 0x202E},    // separators, embeddings
    {.first = 0x2060, .last = 0x2064},    // word joiner, invisible operators
    {.first = 0x2066, .last = 0x2069},    // isolates
    {.first = 0xFDD0, .last = 0xFDEF},    // non-characters
    {.first = 0xFEFF, .last = 0xFEFF},    // byte order mark
    {.first = 0xFFF9, .last = 0xFFFB},    // interlinear annotation
    {.first = 0xE0000, .last = 0xE007F},  // tag characters
}};

bool needs_escape(std::uint32_t code_point) noexcept {
    // U+nFFFE and U+nFFFF of every plane are non-characters.
    if (code_point <= LAST_C1 ||
        (code_point & PLANE_LOW_BITS) == PLANE_LOW_BITS) {
        return true;
    }
    return std::ranges::any_of(HIDDEN, [code_point](const CodeRange& range) {
        return code_point >= range.first && code_point <= range.last;
    });
}

// `\` and `kind`, then `digits` hexadecimal digits of `value`.
std::string hex_escape(char kind, std::uint32_t value, unsigned digits) {
    std::string out = {'\\', kind};
    for (unsigned i = digits; i > 0; --i) {
        out.push_back(
            HEX_DIGITS[(value >> ((i - 1) * NIBBLE_BITS)) & NIBBLE_MASK]);
    }
    return out;
}

// `\uNNNN` for a character of the basic plane, `\UNNNNNNNN` above it.
std::string code_point_escape(std::uint32_t code_point) {
    return code_point <= LAST_BMP ? hex_escape('u', code_point, BMP_DIGITS)
                                  : hex_escape('U', code_point, FULL_DIGITS);
}

// The printable form of the character or byte at `text[where]`, and how
// many bytes of `text` it stands for.
std::pair<std::string, std::size_t> printable_at(std::string_view text,
                                                 std::size_t where) {
    const auto byte = static_cast<unsigned char>(text[where]);
    if (byte < FIRST_NON_ASCII) {
        switch (byte) {
            case '\n':
                return {"\\n", 1};
            case '\r':
                return {"\\r", 1};
            case '\t':
                return {"\\t", 1};
            default:
                break;
        }
        if (byte < FIRST_PRINTABLE || byte == ASCII_DEL) {
            return {hex_escape('x', byte, BYTE_DIGITS), 1};
        }
        return {std::string(1, static_cast<char>(byte)), 1};
    }
    const auto decoded = utf8::decode(text, where);
    if (!decoded) {
        return {hex_escape('x', byte, BYTE_DIGITS), 1};
    }
    if (needs_escape(decoded->second)) {
        return {code_point_escape(decoded->second), decoded->first};
    }
    return {std::string(text.substr(where, decoded->first)), decoded->first};
}

}  // namespace

std::string printable(std::string_view text, std::size_t max_bytes) {
    std::string out;
    const std::size_t room =
        max_bytes > ELLIPSIS.size() ? max_bytes - ELLIPSIS.size() : 0;
    // The length of `out` at the last whole piece that leaves room for the
    // ellipsis.
    std::size_t cut = 0;
    std::size_t where = 0;
    while (where < text.size()) {
        const auto [piece, used] = printable_at(text, where);
        out += piece;
        where += used;
        if (out.size() <= room) {
            cut = out.size();
        }
        if (out.size() > max_bytes) {
            out.resize(cut);
            out += ELLIPSIS;
            return out;
        }
    }
    return out;
}

std::string quote_text(std::string_view text, std::size_t max_bytes) {
    std::string out = "\"";
    bool cut = false;
    if (text.size() > max_bytes) {
        std::size_t end = max_bytes;
        // Back to the start of a character.
        while (end > 0 && (static_cast<unsigned char>(text[end]) &
                           utf8::CONTINUATION_MASK) == utf8::CONTINUATION) {
            --end;
        }
        text = text.substr(0, end);
        cut = true;
    }
    for (const char chr : text) {
        if (chr == '\\' || chr == '"') {
            out.push_back('\\');
        }
        out.push_back(chr);
    }
    if (cut) {
        out += ELLIPSIS;
    }
    out.push_back('"');
    return out;
}

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
    diagnostic.message = printable(diagnostic.message);
    diagnostic.location.file = printable(diagnostic.location.file);
    for (Diagnostic::Note& note : diagnostic.notes) {
        note.kind = printable(note.kind);
        note.location.file = printable(note.location.file);
        note.detail = printable(note.detail);
    }
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
