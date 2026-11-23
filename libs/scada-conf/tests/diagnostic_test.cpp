#include "scada/conf/diagnostic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scada::conf {
namespace {

// Codes used below, by their number.
constexpr std::uint16_t CF_101 = 101;
constexpr std::uint16_t CF_110 = 110;
constexpr std::uint16_t CF_113 = 113;
constexpr std::uint16_t CF_201 = 201;
constexpr std::uint16_t CF_202 = 202;
constexpr std::uint16_t CF_203 = 203;
constexpr std::uint16_t CF_402 = 402;
constexpr std::uint16_t CF_1001 = 1001;
constexpr std::uint16_t CW_510 = 510;

Location loc(std::string file, std::uint32_t line, std::uint32_t column) {
    return Location{.file = std::move(file), .line = line, .column = column};
}

Location line_of(std::string file, std::uint32_t line) {
    return loc(std::move(file), line, 1);
}

// The codes of a report's errors, in report order.
std::vector<std::uint16_t> codes(const Report& report) {
    std::vector<std::uint16_t> out;
    out.reserve(report.errors.size());
    for (const Diagnostic& diagnostic : report.errors) {
        out.push_back(diagnostic.code);
    }
    return out;
}

// The lines of a report's errors, in report order.
std::vector<std::uint32_t> lines(const Report& report) {
    std::vector<std::uint32_t> out;
    out.reserve(report.errors.size());
    for (const Diagnostic& diagnostic : report.errors) {
        out.push_back(diagnostic.location.line);
    }
    return out;
}

std::vector<std::uint32_t> one_to(std::uint32_t last) {
    std::vector<std::uint32_t> out;
    out.reserve(last);
    for (std::uint32_t i = 1; i <= last; ++i) {
        out.push_back(i);
    }
    return out;
}

TEST(Diagnostic, FormatsAsSection14Shows) {
    constexpr std::uint32_t LINE = 14;
    constexpr std::uint32_t COLUMN = 7;
    constexpr std::uint32_t TEMPLATE_LINE = 22;
    constexpr std::uint32_t TEMPLATE_COLUMN = 9;
    const Diagnostic diagnostic{
        .severity = Severity::ERROR,
        .code = CF_402,
        .location = loc("drivers/modbus/line1.yaml", LINE, COLUMN),
        .message = "tag name a.b is 131 bytes, the limit is 127",
        .notes = {Diagnostic::Note{
            .kind = "template",
            .location = loc("templates/x.yaml", TEMPLATE_LINE, TEMPLATE_COLUMN),
            .detail = "point b"}}};
    EXPECT_EQ(format(diagnostic),
              "drivers/modbus/line1.yaml:14:7: error CF-402: tag name a.b is "
              "131 bytes, the limit is 127\n"
              "  from template templates/x.yaml:22:9 (point b)");
}

TEST(Diagnostic, ANoteWithoutDetailHasNoBrackets) {
    const Diagnostic diagnostic{
        .severity = Severity::ERROR,
        .code = CF_402,
        .location = line_of("a.yaml", 2),
        .message = "m",
        .notes = {Diagnostic::Note{.kind = "template",
                                   .location = line_of("b.yaml", 3),
                                   .detail = {}}}};
    EXPECT_EQ(format(diagnostic),
              "a.yaml:2:1: error CF-402: m\n  from template b.yaml:3:1");
}

TEST(Diagnostic, WarningsUseTheirOwnPrefix) {
    const Diagnostic diagnostic{.severity = Severity::WARNING,
                                .code = CW_510,
                                .location = loc("tags/a.yaml", 3, 5),
                                .message = "m",
                                .notes = {}};
    EXPECT_EQ(code_text(diagnostic), "CW-510");
    EXPECT_EQ(format(diagnostic), "tags/a.yaml:3:5: warning CW-510: m");
}

TEST(Diagnostic, OrderIsFileThenLineThenColumnThenCode) {
    constexpr std::uint32_t LATER_LINE = 10;
    Diagnostics diagnostics;
    diagnostics.error(CF_1001, line_of("b.yaml", 1), "");
    diagnostics.error(CF_201, line_of("a.yaml", 2), "");
    diagnostics.error(CF_101, line_of("a.yaml", LATER_LINE), "");
    diagnostics.error(CF_202, line_of("a.yaml", 2), "");
    diagnostics.error(CF_203, loc("a.yaml", 2, 0), "");
    // The code is compared as a number: CF-113 before CF-1001.
    diagnostics.error(CF_113, line_of("b.yaml", 1), "");
    Report report;
    diagnostics.move_into(report);
    EXPECT_EQ(codes(report),
              (std::vector<std::uint16_t>{CF_203, CF_201, CF_202, CF_101,
                                          CF_113, CF_1001}));
}

TEST(Diagnostic, PathsCompareAsBytes) {
    Diagnostics diagnostics;
    diagnostics.error(CF_101, line_of("tags/a.yaml", 1), "");
    diagnostics.error(CF_101, line_of("tags.yaml", 1), "");
    diagnostics.error(CF_101, line_of("tags/A.yaml", 1), "");
    Report report;
    diagnostics.move_into(report);
    ASSERT_EQ(report.errors.size(), 3U);
    // '.' (0x2E) < '/' (0x2F) < 'A' < 'a'
    EXPECT_EQ(report.errors.at(0).location.file, "tags.yaml");
    EXPECT_EQ(report.errors.at(1).location.file, "tags/A.yaml");
    EXPECT_EQ(report.errors.at(2).location.file, "tags/a.yaml");
}

// Section 1.3: the first 1,000 errors in the sorted order, then a count,
// whatever order they were found in.
TEST(Diagnostic, KeepsTheFirstThousandInOrderAndCountsTheRest) {
    constexpr std::uint32_t TOTAL = 4567;
    Diagnostics diagnostics;
    for (std::uint32_t i = TOTAL; i > 0; --i) {
        diagnostics.error(CF_101, line_of("a.yaml", i), "");
    }
    diagnostics.warning(CW_510, line_of("a.yaml", 1), "");
    EXPECT_EQ(diagnostics.error_count(), TOTAL);
    Report report;
    diagnostics.move_into(report);
    EXPECT_EQ(report.error_count, TOTAL);
    EXPECT_EQ(lines(report), one_to(Diagnostics::REPORTED_LIMIT));
    EXPECT_EQ(report.warning_count, 1U);
    EXPECT_EQ(report.warnings.size(), 1U);
    EXPECT_FALSE(passed(report));
}

TEST(Diagnostic, AThousandAndOneGiveAThousandAndACount) {
    constexpr std::uint32_t TOTAL = Diagnostics::REPORTED_LIMIT + 1;
    Diagnostics diagnostics;
    for (std::uint32_t i = 1; i <= TOTAL; ++i) {
        diagnostics.error(CF_101, line_of("a.yaml", i), "");
    }
    Report report;
    diagnostics.move_into(report);
    EXPECT_EQ(report.error_count, TOTAL);
    EXPECT_EQ(lines(report), one_to(Diagnostics::REPORTED_LIMIT));
}

// A later stage appends after the earlier stage's sorted errors, and the
// report as a whole keeps at most 1,000.
TEST(Diagnostic, ALaterStageAppendsWithinTheLimit) {
    constexpr std::uint32_t PER_STAGE = 600;
    constexpr std::uint32_t KEPT_OF_SECOND =
        Diagnostics::REPORTED_LIMIT - PER_STAGE;
    Report report;
    Diagnostics first;
    for (std::uint32_t i = 1; i <= PER_STAGE; ++i) {
        first.error(CF_110, line_of("z.yaml", i), "");
    }
    first.move_into(report);
    Diagnostics second;
    for (std::uint32_t i = 1; i <= PER_STAGE; ++i) {
        second.error(CF_201, line_of("a.yaml", i), "");
    }
    second.move_into(report);
    EXPECT_EQ(report.error_count, 2U * PER_STAGE);
    std::vector<std::uint32_t> expected = one_to(PER_STAGE);
    const std::vector<std::uint32_t> second_lines = one_to(KEPT_OF_SECOND);
    expected.insert(expected.end(), second_lines.begin(), second_lines.end());
    EXPECT_EQ(lines(report), expected);
    EXPECT_EQ(report.errors.front().location.file, "z.yaml");
    EXPECT_EQ(report.errors.back().location.file, "a.yaml");
    EXPECT_EQ(first.error_count(), 0U);
}

TEST(Diagnostic, AReportWithWarningsOnlyPasses) {
    Report report;
    Diagnostics diagnostics;
    diagnostics.warning(CW_510, line_of("a.yaml", 1), "");
    diagnostics.move_into(report);
    EXPECT_TRUE(passed(report));
}

// S1 of the #130 review (D-110): no text of a file reaches a report
// unescaped.
TEST(Diagnostic, PrintableEscapesWhatCouldDriveATerminal) {
    EXPECT_EQ(printable("plain text: a.b_c"), "plain text: a.b_c");
    // A forged second line, a terminal escape and a backslash.
    EXPECT_EQ(printable("x\nsystem.yaml:1:1: error CF-101: y"),
              "x\\nsystem.yaml:1:1: error CF-101: y");
    EXPECT_EQ(printable("\x1b[2J\x1b]0;title\x07"),
              "\\x1B[2J\\x1B]0;title\\x07");
    EXPECT_EQ(printable("a\\nb"), "a\\\\nb");
    EXPECT_EQ(printable("\r\t\x7f"), "\\r\\t\\x7F");
    // Invalid UTF-8, a C1 control character, and characters that reorder
    // or hide text.
    EXPECT_EQ(printable("\xff\xc3"), "\\xFF\\xC3");
    EXPECT_EQ(printable("\xc2\x9b"), "\\u009B");
    EXPECT_EQ(printable("\xe2\x80\xae"), "\\u202E");
    EXPECT_EQ(printable("\xe2\x80\xa8"), "\\u2028");
    EXPECT_EQ(printable("\xef\xbb\xbf"), "\\uFEFF");
    // Other characters are kept.
    EXPECT_EQ(printable("\xc3\xa9t\xc3\xa9 \xe2\x82\xac"),
              "\xc3\xa9t\xc3\xa9 \xe2\x82\xac");
}

TEST(Diagnostic, PrintableCapsTheLength) {
    constexpr std::size_t CAP = 10;
    EXPECT_EQ(printable("0123456789", CAP), "0123456789");
    EXPECT_EQ(printable("0123456789a", CAP), "0123456...");
    // An escape is never cut in two.
    EXPECT_EQ(printable("012345\n789", CAP), "012345...");
    // A character is never cut in two.
    EXPECT_EQ(printable("012345\xc3\xa9\xc3\xa9\xc3\xa9", CAP), "012345...");
    EXPECT_EQ(printable(std::string(MAX_DIAGNOSTIC_TEXT_BYTES * 2, 'x')).size(),
              MAX_DIAGNOSTIC_TEXT_BYTES);
}

TEST(Diagnostic, EveryDiagnosticIsMadePrintable) {
    Diagnostics diagnostics;
    Diagnostic diagnostic{.severity = Severity::ERROR,
                          .code = CF_110,
                          .location = line_of("a\nb.yaml", 1),
                          .message = "duplicate key \"x\ny\"",
                          .notes = {}};
    diagnostic.notes.push_back(Diagnostic::Note{
        .kind = "template\x1b", .location = line_of("t\r.yaml", 2),
        .detail = "point \x1b[31m"});
    diagnostics.add(std::move(diagnostic));
    Report report;
    diagnostics.move_into(report);
    ASSERT_EQ(report.errors.size(), 1U);
    EXPECT_EQ(format(report.errors.front()),
              "a\\nb.yaml:1:1: error CF-110: duplicate key \"x\\ny\"\n"
              "  from template\\x1B t\\r.yaml:2:1 (point \\x1B[31m)");
}

}  // namespace
}  // namespace scada::conf
