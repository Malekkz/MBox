// Unit tests for what scadactl shows, in both forms, without shared
// memory: rows are built here as the database would give them.

#include "scada/ctl/view.h"
#include "scada/ctl/database.h"

#include "scada/tag/scada_tag.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace scada::ctl {
namespace {

constexpr std::int64_t SOME_MONO_NS = 5'000'000'000;
constexpr std::int64_t AGE_12_MS = 12'000'000;
constexpr std::int64_t SOURCE_NS = 1'794'738'030'123'000'000;
constexpr double LEVEL = 4.25;
constexpr std::uint32_t PID = 4321;
constexpr std::uint32_t TIMEOUT_MS = 3000;
constexpr std::uint64_t GENERATION = 7;
constexpr std::uint64_t HEARTBEATS = 9;

TagRow good_row() {
    TagRow row;
    row.name = "plant.level";
    row.owner = "io";
    row.type = SCADA_TYPE_FLOAT64;
    row.value.type = SCADA_TYPE_FLOAT64;
    row.value.quality = SCADA_QUALITY_GOOD;
    row.value.value = std::bit_cast<std::uint64_t>(LEVEL);
    row.value.update_mono_ns = SOME_MONO_NS;
    row.value.update_time_ns = SOURCE_NS;
    row.value.source_time_ns = SOURCE_NS;
    row.value.age_ns = AGE_12_MS;
    row.value.change_count = 3;
    return row;
}

TagRow stale_row() {
    TagRow row = good_row();
    row.name = "plant.flow";
    row.value.quality_flags = SCADA_FLAG_STALE;
    return row;
}

TagRow bad_row() {
    TagRow row = good_row();
    row.name = "plant.temp";
    row.owner = "probe";
    row.value = {};
    row.value.type = SCADA_TYPE_FLOAT64;
    row.value.quality = SCADA_QUALITY_BAD;
    row.value.quality_flags = SCADA_FLAG_OWNER_DOWN;
    return row;
}

// The lines of `text`.
std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        lines.emplace_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{}
                                             : text.substr(end + 1);
    }
    return lines;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

TEST(TagTable, MarksBadAndStaleRows) {
    const std::vector<TagRow> rows{good_row(), stale_row(), bad_row()};
    std::string out;
    render_tag_table(out, rows);
    const std::vector<std::string> lines = lines_of(out);
    ASSERT_EQ(lines.size(), 4U) << out;
    EXPECT_TRUE(lines[0].starts_with("  NAME ")) << out;
    EXPECT_TRUE(lines[1].starts_with("  plant.level ")) << out;
    EXPECT_TRUE(lines[2].starts_with("~ plant.flow ")) << out;
    EXPECT_TRUE(lines[3].starts_with("! plant.temp ")) << out;
    EXPECT_TRUE(contains(lines[1], " 4.25 ")) << out;
    EXPECT_TRUE(contains(lines[1], "0.012 s")) << out;
    EXPECT_TRUE(contains(lines[2], " STALE ")) << out;
    EXPECT_TRUE(contains(lines[3], " - ")) << out;
    EXPECT_TRUE(contains(lines[3], "OWNER_DOWN")) << out;
}

TEST(TagTable, ColumnsLineUp) {
    const std::vector<TagRow> rows{good_row(), bad_row()};
    std::string out;
    render_tag_table(out, rows);
    const std::vector<std::string> lines = lines_of(out);
    ASSERT_EQ(lines.size(), 3U);
    const std::size_t type_column = lines[0].find("TYPE");
    EXPECT_EQ(lines[1].find("float64"), type_column) << out;
    EXPECT_EQ(lines[2].find("float64"), type_column) << out;
}

TEST(TagTable, ARowThatCouldNotBeReadIsBad) {
    TagRow row = good_row();
    row.read_code = SCADA_E_BUSY;
    std::string out;
    render_tag_table(out, std::vector<TagRow>{row});
    EXPECT_TRUE(lines_of(out).at(1).starts_with("! plant.level ")) << out;
    EXPECT_TRUE(contains(out, " busy ")) << out;
    EXPECT_FALSE(contains(out, "4.25")) << out;
}

TEST(TagJson, CarriesBadStaleAndTheValue) {
    std::string out;
    render_tag_json(out, stale_row());
    EXPECT_EQ(out,
              R"({"name":"plant.flow","type":"float64","owner":"io",)"
              R"("value":4.25,"quality":"GOOD","flags":["STALE"],"bad":false,)"
              R"("stale":true,"read":"ok","age_ns":12000000,)"
              R"("source_time_ns":1794738030123000000,)"
              R"("update_time_ns":1794738030123000000,"change_count":3})");
    out.clear();
    render_tag_json(out, bad_row());
    EXPECT_TRUE(contains(out, R"("value":null)")) << out;
    EXPECT_TRUE(contains(out, R"("bad":true)")) << out;
    EXPECT_TRUE(contains(out, R"("age_ns":null)")) << out;
    EXPECT_TRUE(contains(out, R"("flags":["OWNER_DOWN"])")) << out;
}

TEST(TagJson, EscapesStringValues) {
    TagRow row = good_row();
    row.type = SCADA_TYPE_STRING;
    row.value.type = SCADA_TYPE_STRING;
    row.text = "a\"b\n\x1b";
    std::string out;
    render_tag_json(out, row);
    EXPECT_TRUE(contains(out, R"("value":"a\"b\n\u001b")")) << out;
    out.clear();
    render_tag_table(out, std::vector<TagRow>{row});
    EXPECT_TRUE(contains(out, R"("a\"b\n\x1b")")) << out;
    EXPECT_FALSE(contains(out, "\x1b")) << out;
}

TEST(TagList, JsonHoldsTheGenerationAndEveryRow) {
    const std::vector<TagRow> rows{good_row(), bad_row()};
    std::string out;
    render_tag_list_json(out, GENERATION, rows);
    EXPECT_TRUE(
        out.starts_with(R"({"generation":7,"tags":[{"name":"plant.level")"))
        << out;
    EXPECT_TRUE(out.ends_with("}]}\n")) << out;
    out.clear();
    render_tag_list_json(out, 1, {});
    EXPECT_EQ(out, "{\"generation\":1,\"tags\":[]}\n");
}

TEST(TagDetail, OneFieldPerLineWithTheMark) {
    std::string out;
    render_tag_detail(out, stale_row());
    EXPECT_TRUE(contains(out, "name    plant.flow\n")) << out;
    EXPECT_TRUE(contains(out, "quality ~ GOOD\n")) << out;
    EXPECT_TRUE(contains(out, "flags   STALE\n")) << out;
    EXPECT_TRUE(contains(out, "age     0.012 s\n")) << out;
    EXPECT_TRUE(contains(out, "source  2026-11-15T10:20:30.123Z\n")) << out;
    out.clear();
    render_tag_detail(out, bad_row());
    EXPECT_TRUE(contains(out, "quality ! BAD\n")) << out;
    EXPECT_TRUE(contains(out, "value   -\n")) << out;
    EXPECT_TRUE(contains(out, "source  -\n")) << out;
}

StatusView sample_status() {
    StatusView view;
    view.status.generation = 2;
    view.status.generation_state = SCADA_GENERATION_RUNNING;
    ProcessRow feeder;
    std::ranges::copy(std::string_view("io"), std::begin(feeder.status.name));
    feeder.status.state = SCADA_PROC_RUNNING;
    feeder.status.heartbeat = HEARTBEATS;
    feeder.status.heartbeat_mono_ns = SOME_MONO_NS;
    feeder.status.heartbeat_age_ns = AGE_12_MS;
    feeder.status.heartbeat_timeout_ms = TIMEOUT_MS;
    feeder.status.pid = PID;
    ProcessRow probe;
    std::ranges::copy(std::string_view("probe"), std::begin(probe.status.name));
    probe.status.process_index = 1;
    probe.status.down = 1;
    probe.status.down_reason = SCADA_DOWN_REGION_MISSING;
    probe.status.marked_down = 1;
    probe.status.heartbeat_timeout_ms = TIMEOUT_MS;
    view.processes = {feeder, probe};
    OwnerRow io_owner;
    std::ranges::copy(std::string_view("io"), std::begin(io_owner.info.name));
    io_owner.info.tag_count = 1;
    io_owner.process = "io";
    OwnerRow probe_owner;
    std::ranges::copy(std::string_view("probe"),
                      std::begin(probe_owner.info.name));
    probe_owner.info.tag_count = 2;
    probe_owner.process = "probe";
    probe_owner.owner_down = true;
    view.owners = {io_owner, probe_owner};
    return view;
}

TEST(Status, TextMarksWhatIsDown) {
    std::string out;
    render_status_text(out, sample_status());
    const std::vector<std::string> lines = lines_of(out);
    ASSERT_GE(lines.size(), 9U) << out;
    EXPECT_EQ(lines[0], "generation 2 RUNNING");
    EXPECT_EQ(lines[1], "  scada-tagd up");
    EXPECT_TRUE(lines[4].starts_with("  io ")) << out;
    EXPECT_TRUE(contains(lines[4], "RUNNING")) << out;
    EXPECT_TRUE(contains(lines[4], "4321")) << out;
    EXPECT_TRUE(lines[5].starts_with("! probe ")) << out;
    EXPECT_TRUE(contains(lines[5], "region missing")) << out;
    EXPECT_TRUE(lines[8].starts_with("  io ")) << out;
    EXPECT_TRUE(lines[9].starts_with("! probe ")) << out;
    EXPECT_TRUE(lines[9].ends_with("yes")) << out;
}

TEST(Status, TextReportsTagdDownAClockSystemCallAndAGenerationChange) {
    StatusView view = sample_status();
    view.status.tagd_down = 1;
    view.status.clock_syscall = 1;
    view.status_code = SCADA_E_RETIRING;
    std::string out;
    render_status_text(out, view);
    EXPECT_TRUE(contains(out, "newer generation")) << out;
    EXPECT_TRUE(contains(out, "! scada-tagd down\n")) << out;
    EXPECT_TRUE(contains(out, "! reading the clocks is a system call")) << out;
}

TEST(Status, Json) {
    std::string out;
    render_status_json(out, sample_status());
    EXPECT_TRUE(out.starts_with(
        R"({"generation":2,"state":"RUNNING","generation_changed":false,)"
        R"("tagd_down":false,"clock_syscall":false,"processes":[)"))
        << out;
    EXPECT_TRUE(contains(out, R"("name":"probe","index":1,"state":"MISSING",)"
                              R"("down":true,"down_reason":"region missing")"))
        << out;
    EXPECT_TRUE(contains(out, R"("heartbeat_age_ns":null)")) << out;
    EXPECT_TRUE(contains(out, R"("pid":4321})")) << out;
    EXPECT_TRUE(contains(out, R"({"name":"probe","process":"probe","tags":2,)"
                              R"("owner_down":true})"))
        << out;
    EXPECT_TRUE(out.ends_with("]}\n")) << out;
}

TEST(Watch, TextLines) {
    std::string out;
    render_watch_text(out, WatchEvent::NOW, good_row());
    EXPECT_EQ(out, "  now    plant.level  4.25  GOOD  -  age 0.012 s\n");
    out.clear();
    render_watch_text(out, WatchEvent::CHANGE, stale_row());
    EXPECT_EQ(out, "~ change plant.flow  4.25  GOOD  STALE  age 0.012 s\n");
    out.clear();
    render_watch_text(out, WatchEvent::NOW, bad_row());
    EXPECT_EQ(out, "! now    plant.temp  -  BAD  OWNER_DOWN  age -\n");
}

TEST(Watch, JsonLines) {
    std::string out;
    render_watch_json(out, WatchEvent::CHANGE, bad_row());
    EXPECT_TRUE(out.starts_with(R"({"event":"change","name":"plant.temp",)"))
        << out;
    EXPECT_TRUE(contains(out, R"("bad":true)")) << out;
    EXPECT_TRUE(out.ends_with("}\n")) << out;
    EXPECT_EQ(std::ranges::count(out, '\n'), 1);
}

TEST(Watch, GapAndGenerationLines) {
    std::string out;
    render_gap_text(out, "io", SCADA_GAP_OWNER_DOWN);
    EXPECT_EQ(out, "! gap     owner io: owner down; its tags are read again\n");
    out.clear();
    render_gap_json(out, "io", SCADA_GAP_OVERRUN);
    EXPECT_EQ(out,
              "{\"event\":\"gap\",\"owner\":\"io\",\"reason\":\"overrun\"}\n");
    out.clear();
    render_generation_text(out, 3);
    EXPECT_EQ(out, "! generation changed: now 3; every tag is read again\n");
    out.clear();
    render_generation_json(out, 3);
    EXPECT_EQ(out, "{\"event\":\"generation\",\"generation\":3}\n");
}

TEST(Names, OfGapsStatesAndReasons) {
    EXPECT_EQ(gap_reason_name(SCADA_GAP_RESTART, true), "restart");
    EXPECT_EQ(gap_reason_name(SCADA_GAP_RING, true), "ring");
    EXPECT_EQ(gap_reason_name(SCADA_GAP_OWNER_BACK, true), "owner_back");
    EXPECT_EQ(gap_reason_name(SCADA_GAP_NONE, true), "unknown");
    EXPECT_EQ(gap_reason_name(SCADA_GAP_OWNER_BACK, false), "owner back");
    EXPECT_EQ(process_state_name(0), "MISSING");
    EXPECT_EQ(process_state_name(SCADA_PROC_STARTING), "STARTING");
    EXPECT_EQ(process_state_name(SCADA_PROC_STOPPING), "STOPPING");
    EXPECT_EQ(process_state_name(4), "UNKNOWN");
    EXPECT_EQ(down_reason_name(SCADA_DOWN_HEARTBEAT_STALE), "heartbeat stale");
    EXPECT_EQ(down_reason_name(SCADA_DOWN_NOT_RUNNING), "not running");
    EXPECT_EQ(down_reason_name(4), "unknown");
}

TEST(ObserveErrors, SayWhatToDo) {
    EXPECT_TRUE(contains(describe_observe_error(SCADA_E_PERMISSION, 0, ""),
                         "member of the group scada"));
    EXPECT_TRUE(contains(describe_observe_error(SCADA_E_PERMISSION, 0, ""),
                         "/dev/shm/scada"));
    EXPECT_TRUE(contains(describe_observe_error(SCADA_E_IO, ENOENT, "/x"),
                         "no tag database in /x: is scada-tagd running?"));
    EXPECT_TRUE(contains(describe_observe_error(SCADA_E_IO, EAGAIN, "/x"),
                         "not running yet"));
    EXPECT_TRUE(contains(describe_observe_error(SCADA_E_LAYOUT, 0, "/x"),
                         "layout version"));
    EXPECT_TRUE(contains(describe_observe_error(SCADA_E_RETIRING, 0, "/x"),
                         "try again"));
    // A directory name is shown so that a terminal only shows it.
    EXPECT_FALSE(
        contains(describe_observe_error(SCADA_E_IO, ENOENT, "/\x1b"), "\x1b"));
}

}  // namespace
}  // namespace scada::ctl
