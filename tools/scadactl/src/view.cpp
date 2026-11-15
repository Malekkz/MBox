#include "scada/ctl/view.h"

#include "scada/ctl/format.h"
#include "scada/tag/scada_tag.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::ctl {

namespace {

constexpr std::string_view NONE = "-";
constexpr std::string_view COLUMN_GAP = "  ";

// What a read that failed gives instead of a value.
std::string_view read_problem(int code) {
    switch (code) {
        case SCADA_OK:
            return "ok";
        case SCADA_E_BUSY:
            return "busy";
        case SCADA_E_LAYOUT:
            return "invalid record";
        case SCADA_E_INVALID:
            return "not resolved";
        default:
            return "error";
    }
}

Mark row_mark(const TagRow& row) {
    if (row.read_code != SCADA_OK) {
        return Mark::BAD;
    }
    return mark_of(row.value.quality, row.value.quality_flags);
}

bool row_has_value(const TagRow& row) {
    return row.read_code == SCADA_OK && has_value(row.value);
}

std::string value_text(const TagRow& row) {
    std::string out;
    if (row.read_code != SCADA_OK) {
        out.append(NONE);
        return out;
    }
    append_value_text(out, row.value, row.text);
    return out;
}

std::string flags_text(std::uint16_t flags) {
    std::string out;
    append_flag_names(out, flags, ",");
    if (out.empty()) {
        out.append(NONE);
    }
    return out;
}

std::string age_text(const TagRow& row) {
    std::string out;
    if (row_has_value(row)) {
        append_age(out, row.value.age_ns);
    } else {
        out.append(NONE);
    }
    return out;
}

std::string safe_text(std::string_view text) {
    std::string out;
    append_terminal_safe(out, text);
    return out;
}

void append_padded(std::string& out, std::string_view text, std::size_t width) {
    out.append(text);
    if (text.size() < width) {
        out.append(width - text.size(), ' ');
    }
}

// A table: a header line, then one line per row, each starting with its
// mark and a space; every column but the last padded to its widest cell.
void append_table(std::string& out, std::span<const std::string_view> headers,
                  const std::vector<std::vector<std::string>>& rows,
                  std::span<const char> marks) {
    std::vector<std::size_t> widths(headers.size());
    for (std::size_t column = 0; column < headers.size(); ++column) {
        widths[column] = headers[column].size();
        for (const std::vector<std::string>& row : rows) {
            widths[column] = std::max(widths[column], row.at(column).size());
        }
    }
    const auto line = [&](char mark, const auto& cells) {
        out.push_back(mark);
        out.push_back(' ');
        for (std::size_t column = 0; column < headers.size(); ++column) {
            const std::string_view cell = cells[column];
            if (column + 1 == headers.size()) {
                out.append(cell);
            } else {
                append_padded(out, cell, widths[column]);
                out.append(COLUMN_GAP);
            }
        }
        out.push_back('\n');
    };
    line(' ', headers);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        line(marks[index], rows[index]);
    }
}

void append_json_key(std::string& out, std::string_view key) {
    append_json_string(out, key);
    out.push_back(':');
}

template <typename T>
void append_json_number(std::string& out, std::string_view key, T value) {
    append_json_key(out, key);
    out.append(std::to_string(value));
}

void append_json_bool(std::string& out, std::string_view key, bool value) {
    append_json_key(out, key);
    out.append(value ? "true" : "false");
}

void append_json_text(std::string& out, std::string_view key,
                      std::string_view value) {
    append_json_key(out, key);
    append_json_string(out, value);
}

void append_json_flags(std::string& out, std::uint16_t flags) {
    append_json_key(out, "flags");
    std::string names;
    append_flag_names(names, flags, "\n");
    out.push_back('[');
    std::string_view rest = names;
    bool first = true;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        if (!first) {
            out.push_back(',');
        }
        first = false;
        append_json_string(out, rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{}
                                             : rest.substr(end + 1);
    }
    out.push_back(']');
}

// The fields of a tag as JSON, without the braces.
void append_tag_fields(std::string& out, const TagRow& row) {
    const bool value_there = row_has_value(row);
    append_json_text(out, "name", row.name);
    out.push_back(',');
    append_json_text(out, "type", type_name(row.type));
    out.push_back(',');
    append_json_text(out, "owner", row.owner);
    out.push_back(',');
    append_json_key(out, "value");
    if (row.read_code == SCADA_OK) {
        append_value_json(out, row.value, row.text);
    } else {
        out.append("null");
    }
    out.push_back(',');
    append_json_text(out, "quality",
                     row.read_code == SCADA_OK
                         ? quality_name(row.value.quality)
                         : quality_name(SCADA_QUALITY_BAD));
    out.push_back(',');
    append_json_flags(out, row.read_code == SCADA_OK ? row.value.quality_flags
                                                     : std::uint16_t{0});
    out.push_back(',');
    append_json_bool(out, "bad", row_mark(row) == Mark::BAD);
    out.push_back(',');
    append_json_bool(out, "stale",
                     row.read_code == SCADA_OK &&
                         (row.value.quality_flags & SCADA_FLAG_STALE) != 0);
    out.push_back(',');
    append_json_text(out, "read", read_problem(row.read_code));
    out.push_back(',');
    append_json_key(out, "age_ns");
    out.append(value_there ? std::to_string(row.value.age_ns) : "null");
    out.push_back(',');
    append_json_key(out, "source_time_ns");
    out.append(value_there ? std::to_string(row.value.source_time_ns) : "null");
    out.push_back(',');
    append_json_key(out, "update_time_ns");
    out.append(value_there && row.value.update_time_ns != 0
                   ? std::to_string(row.value.update_time_ns)
                   : "null");
    out.push_back(',');
    append_json_number(out, "change_count", row.value.change_count);
}

std::string_view generation_state_name(std::uint32_t state) {
    switch (state) {
        case SCADA_GENERATION_RUNNING:
            return "RUNNING";
        case SCADA_GENERATION_RETIRING:
            return "RETIRING";
        default:
            return "INVALID";
    }
}

std::string_view event_name(WatchEvent event) {
    return event == WatchEvent::NOW ? "now" : "change";
}

}  // namespace

std::string_view gap_reason_name(std::uint8_t reason, bool json) {
    switch (reason) {
        case SCADA_GAP_OVERRUN:
            return json ? "overrun" : "changes lost: the watch fell behind";
        case SCADA_GAP_RESTART:
            return json ? "restart" : "owner restarted";
        case SCADA_GAP_RING:
            return json ? "ring" : "change ring appeared, went or was replaced";
        case SCADA_GAP_OWNER_DOWN:
            return json ? "owner_down" : "owner down";
        case SCADA_GAP_OWNER_BACK:
            return json ? "owner_back" : "owner back";
        default:
            return json ? "unknown" : "unknown reason";
    }
}

std::string_view process_state_name(std::uint32_t state) {
    switch (state) {
        case 0:
            return "MISSING";
        case SCADA_PROC_STARTING:
            return "STARTING";
        case SCADA_PROC_RUNNING:
            return "RUNNING";
        case SCADA_PROC_STOPPING:
            return "STOPPING";
        default:
            return "UNKNOWN";
    }
}

std::string_view down_reason_name(std::uint8_t reason) {
    switch (reason) {
        case SCADA_DOWN_NONE:
            return "up";
        case SCADA_DOWN_REGION_MISSING:
            return "region missing";
        case SCADA_DOWN_NOT_RUNNING:
            return "not running";
        case SCADA_DOWN_HEARTBEAT_STALE:
            return "heartbeat stale";
        default:
            return "unknown";
    }
}

void render_tag_table(std::string& out, std::span<const TagRow> rows) {
    constexpr std::array<std::string_view, 7> HEADERS{
        "NAME", "TYPE", "VALUE", "QUALITY", "FLAGS", "AGE", "OWNER"};
    std::vector<std::vector<std::string>> cells;
    std::vector<char> marks;
    for (const TagRow& row : rows) {
        cells.push_back(
            {safe_text(row.name), std::string(type_name(row.type)),
             value_text(row),
             std::string(row.read_code == SCADA_OK
                             ? quality_name(row.value.quality)
                             : read_problem(row.read_code)),
             flags_text(row.read_code == SCADA_OK ? row.value.quality_flags
                                                  : std::uint16_t{0}),
             age_text(row), safe_text(row.owner)});
        marks.push_back(mark_char(row_mark(row)));
    }
    append_table(out, HEADERS, cells, marks);
}

void render_tag_json(std::string& out, const TagRow& row) {
    out.push_back('{');
    append_tag_fields(out, row);
    out.push_back('}');
}

void render_tag_list_json(std::string& out, std::uint64_t generation,
                          std::span<const TagRow> rows) {
    out.push_back('{');
    append_json_number(out, "generation", generation);
    out.append(",\"tags\":[");
    bool first = true;
    for (const TagRow& row : rows) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        render_tag_json(out, row);
    }
    out.append("]}\n");
}

void render_tag_detail(std::string& out, const TagRow& row) {
    const auto field = [&out](std::string_view name, std::string_view value) {
        append_padded(out, name, sizeof("quality"));
        out.append(value);
        out.push_back('\n');
    };
    field("name", safe_text(row.name));
    field("type", type_name(row.type));
    field("owner", safe_text(row.owner));
    field("value", value_text(row));
    std::string quality(1, mark_char(row_mark(row)));
    quality.push_back(' ');
    quality.append(row.read_code == SCADA_OK ? quality_name(row.value.quality)
                                             : read_problem(row.read_code));
    field("quality", quality);
    field("flags",
          flags_text(row.read_code == SCADA_OK ? row.value.quality_flags
                                               : std::uint16_t{0}));
    field("age", age_text(row));
    std::string time;
    append_utc_time(time, row_has_value(row) ? row.value.source_time_ns : 0);
    field("source", time);
    time.clear();
    append_utc_time(time, row_has_value(row) ? row.value.update_time_ns : 0);
    field("updated", time);
    field("changes", std::to_string(row.value.change_count));
}

void render_status_text(std::string& out, const StatusView& view) {
    const scada_status& status = view.status;
    out.append("generation ");
    out.append(std::to_string(status.generation));
    out.push_back(' ');
    out.append(generation_state_name(status.generation_state));
    out.append(view.status_code == SCADA_E_RETIRING &&
                       status.generation_state == SCADA_GENERATION_RUNNING
                   ? " (current names a newer generation)\n"
                   : "\n");
    out.append(status.tagd_down != 0 ? "! scada-tagd down\n"
                                     : "  scada-tagd up\n");
    if (status.clock_syscall != 0) {
        out.append(
            "! reading the clocks is a system call on this machine: real-time "
            "functions are not real-time safe\n");
    }
    out.push_back('\n');
    constexpr std::array<std::string_view, 7> PROCESS_HEADERS{
        "PROCESS", "STATE",     "LIVENESS", "HEARTBEAT AGE",
        "TIMEOUT", "TAGD FLAG", "PID"};
    std::vector<std::vector<std::string>> cells;
    std::vector<char> marks;
    for (const ProcessRow& row : view.processes) {
        const scada_proc_status_t& proc = row.status;
        std::string age;
        if (proc.heartbeat_mono_ns != 0) {
            append_age(age, proc.heartbeat_age_ns);
        } else {
            age.append(NONE);
        }
        cells.push_back(
            {safe_text(std::data(proc.name)),
             std::string(process_state_name(proc.state)),
             std::string(down_reason_name(proc.down_reason)), age,
             std::to_string(proc.heartbeat_timeout_ms) + " ms",
             proc.marked_down != 0 ? "down" : "up",
             proc.pid != 0 ? std::to_string(proc.pid) : std::string(NONE)});
        marks.push_back(proc.down != 0 ? '!' : ' ');
    }
    append_table(out, PROCESS_HEADERS, cells, marks);

    out.push_back('\n');
    constexpr std::array<std::string_view, 4> OWNER_HEADERS{
        "OWNER", "PROCESS", "TAGS", "OWNER_DOWN"};
    cells.clear();
    marks.clear();
    for (const OwnerRow& row : view.owners) {
        cells.push_back({safe_text(std::data(row.info.name)),
                         safe_text(row.process),
                         std::to_string(row.info.tag_count),
                         row.owner_down ? "yes" : "no"});
        marks.push_back(row.owner_down ? '!' : ' ');
    }
    append_table(out, OWNER_HEADERS, cells, marks);
}

void render_status_json(std::string& out, const StatusView& view) {
    const scada_status& status = view.status;
    out.push_back('{');
    append_json_number(out, "generation", status.generation);
    out.push_back(',');
    append_json_text(out, "state",
                     generation_state_name(status.generation_state));
    out.push_back(',');
    append_json_bool(out, "generation_changed",
                     view.status_code == SCADA_E_RETIRING);
    out.push_back(',');
    append_json_bool(out, "tagd_down", status.tagd_down != 0);
    out.push_back(',');
    append_json_bool(out, "clock_syscall", status.clock_syscall != 0);
    out.append(",\"processes\":[");
    bool first = true;
    for (const ProcessRow& row : view.processes) {
        const scada_proc_status_t& proc = row.status;
        out.append(first ? "{" : ",{");
        first = false;
        append_json_text(out, "name", std::data(proc.name));
        out.push_back(',');
        append_json_number(out, "index", proc.process_index);
        out.push_back(',');
        append_json_text(out, "state", process_state_name(proc.state));
        out.push_back(',');
        append_json_bool(out, "down", proc.down != 0);
        out.push_back(',');
        append_json_text(out, "down_reason",
                         down_reason_name(proc.down_reason));
        out.push_back(',');
        append_json_bool(out, "marked_down", proc.marked_down != 0);
        out.push_back(',');
        append_json_number(out, "heartbeat", proc.heartbeat);
        out.push_back(',');
        append_json_key(out, "heartbeat_age_ns");
        out.append(proc.heartbeat_mono_ns != 0
                       ? std::to_string(proc.heartbeat_age_ns)
                       : "null");
        out.push_back(',');
        append_json_number(out, "heartbeat_timeout_ms",
                           proc.heartbeat_timeout_ms);
        out.push_back(',');
        append_json_bool(out, "realtime", proc.realtime != 0);
        out.push_back(',');
        append_json_number(out, "uid", proc.uid);
        out.push_back(',');
        append_json_key(out, "pid");
        out.append(proc.pid != 0 ? std::to_string(proc.pid) : "null");
        out.push_back('}');
    }
    out.append("],\"owners\":[");
    first = true;
    for (const OwnerRow& row : view.owners) {
        out.append(first ? "{" : ",{");
        first = false;
        append_json_text(out, "name", std::data(row.info.name));
        out.push_back(',');
        append_json_text(out, "process", row.process);
        out.push_back(',');
        append_json_number(out, "tags", row.info.tag_count);
        out.push_back(',');
        append_json_bool(out, "owner_down", row.owner_down);
        out.push_back('}');
    }
    out.append("]}\n");
}

void render_watch_text(std::string& out, WatchEvent event, const TagRow& row) {
    out.push_back(mark_char(row_mark(row)));
    out.push_back(' ');
    append_padded(out, event_name(event), sizeof("change"));
    out.append(safe_text(row.name));
    out.append(COLUMN_GAP);
    out.append(value_text(row));
    out.append(COLUMN_GAP);
    out.append(row.read_code == SCADA_OK ? quality_name(row.value.quality)
                                         : read_problem(row.read_code));
    out.append(COLUMN_GAP);
    out.append(flags_text(row.read_code == SCADA_OK ? row.value.quality_flags
                                                    : std::uint16_t{0}));
    out.append(COLUMN_GAP);
    out.append("age ");
    out.append(age_text(row));
    out.push_back('\n');
}

void render_watch_json(std::string& out, WatchEvent event, const TagRow& row) {
    out.push_back('{');
    append_json_text(out, "event", event_name(event));
    out.push_back(',');
    append_tag_fields(out, row);
    out.append("}\n");
}

void render_gap_text(std::string& out, std::string_view owner,
                     std::uint8_t reason) {
    out.append("! gap     owner ");
    out.append(safe_text(owner));
    out.append(": ");
    out.append(gap_reason_name(reason, false));
    out.append("; its tags are read again\n");
}

void render_gap_json(std::string& out, std::string_view owner,
                     std::uint8_t reason) {
    out.push_back('{');
    append_json_text(out, "event", "gap");
    out.push_back(',');
    append_json_text(out, "owner", owner);
    out.push_back(',');
    append_json_text(out, "reason", gap_reason_name(reason, true));
    out.append("}\n");
}

void render_generation_text(std::string& out, std::uint64_t generation) {
    out.append("! generation changed: now ");
    out.append(std::to_string(generation));
    out.append("; every tag is read again\n");
}

void render_generation_json(std::string& out, std::uint64_t generation) {
    out.push_back('{');
    append_json_text(out, "event", "generation");
    out.push_back(',');
    append_json_number(out, "generation", generation);
    out.append("}\n");
}

}  // namespace scada::ctl
