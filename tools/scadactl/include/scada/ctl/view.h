#ifndef SCADA_CTL_VIEW_H
#define SCADA_CTL_VIEW_H

// What scadactl shows, as plain values, and how it is written: as text for
// a person, or as JSON. Filled from the tag database by database.h; kept
// apart from it so that every form can be tested without shared memory.

#include "scada/tag/scada_tag.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scada::ctl {

// One tag as it was read.
struct TagRow {
    std::string name;
    std::string owner;
    std::uint8_t type = 0;  // SCADA_TYPE_*
    // The return code of the read: SCADA_OK, or the error that left no
    // value (SCADA_E_BUSY: the owner was mid-write at every attempt).
    int read_code = SCADA_OK;
    scada_value value{};
    std::string text;  // the value of a STRING tag
};

// One process of the process table, as scada_proc_status reported it.
struct ProcessRow {
    scada_proc_status_t status{};
};

// One owner, with whether its process is down.
struct OwnerRow {
    scada_owner_info_t info{};
    std::string process;
    bool owner_down = false;
};

// What `scadactl status` shows. Neither form shows a uid or a pid: they
// identify accounts and processes of the unit and are not for every
// reader of the output.
struct StatusView {
    scada_status status{};
    int status_code = SCADA_OK;  // of scada_tag_status
    std::vector<ProcessRow> processes;
    std::vector<OwnerRow> owners;
};

// A line of `tag watch`.
enum class WatchEvent : std::uint8_t {
    NOW,     // the value read when the watch started or after a gap
    CHANGE,  // a change as its owner wrote it
};

// The name of a reason of a gap (scada_sub_stats_t.last_gap), for example
// "owner down"; with `json`, in the form of a JSON key, for example
// "owner_down". The view refers to static storage.
[[nodiscard]] std::string_view gap_reason_name(std::uint8_t reason, bool json);

// The name of a process state (SCADA_PROC_*), or "MISSING" for 0 and
// "UNKNOWN" for a number that is no state.
[[nodiscard]] std::string_view process_state_name(std::uint32_t state);

// The name of a reason of scada_proc_status_t.down_reason, for example
// "heartbeat stale".
[[nodiscard]] std::string_view down_reason_name(std::uint8_t reason);

// The table of `tag list`: a header line, then one line per row with mark,
// name, type, value, quality, flags, age and owner.
void render_tag_table(std::string& out, std::span<const TagRow> rows);

// The fields of `row` as one JSON object.
void render_tag_json(std::string& out, const TagRow& row);

// The line that `tag list` and `tag get` put first when scada-tagd is down:
// its liveness check has stopped, so a value may be stale without a mark
// (contract 10.1).
void render_tagd_warning(std::string& out);

// `tag list --json`: {"generation": ..., "tagd_down": ..., "tags": [...]}.
void render_tag_list_json(std::string& out, std::uint64_t generation,
                          bool tagd_down, std::span<const TagRow> rows);

// `tag get`: one field per line.
void render_tag_detail(std::string& out, const TagRow& row);

// `tag get --json`: the fields of render_tag_json and "tagd_down".
void render_tag_get_json(std::string& out, const TagRow& row, bool tagd_down);

// `status`, for a person.
void render_status_text(std::string& out, const StatusView& view);

// `status --json`.
void render_status_json(std::string& out, const StatusView& view);

// One line of `tag watch` for the tag `row`, ending in a newline.
void render_watch_text(std::string& out, WatchEvent event, const TagRow& row);
void render_watch_json(std::string& out, WatchEvent event, const TagRow& row);

// One line of `tag watch` for a gap of the owner `owner`, ending in a
// newline.
void render_gap_text(std::string& out, std::string_view owner,
                     std::uint8_t reason);
void render_gap_json(std::string& out, std::string_view owner,
                     std::uint8_t reason);

// One line of `tag watch` when scada-tagd goes down or comes back, ending
// in a newline.
void render_tagd_text(std::string& out, bool down);
void render_tagd_json(std::string& out, bool down);

// One line of `tag watch` for a change of generation, ending in a newline.
void render_generation_text(std::string& out, std::uint64_t generation);
void render_generation_json(std::string& out, std::uint64_t generation);

}  // namespace scada::ctl

#endif  // SCADA_CTL_VIEW_H
