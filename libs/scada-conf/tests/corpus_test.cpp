// The conformance corpus of configuration schemas contract section 14: each
// case is a tree and the list of (file, line, column, code) it must give, in
// the order of section 1.4. See corpus/README.md.

#include "scada/common/result.h"
#include "scada/conf/diagnostic.h"
#include "scada/conf/validate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace scada::conf {
namespace {

namespace fs = std::filesystem;

std::vector<fs::path> case_directories() {
    std::vector<fs::path> cases;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(SCADA_CONF_CORPUS_DIR)) {
        if (entry.is_directory()) {
            cases.push_back(entry.path());
        }
    }
    std::ranges::sort(cases);
    return cases;
}

std::vector<std::string> expected_lines(const fs::path& file) {
    std::ifstream input(file);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() != '#') {
            lines.push_back(line);
        }
    }
    return lines;
}

// `<file>:<line>:<column>: <code>`, the part of a diagnostic the corpus
// pins.
std::string corpus_line(const Diagnostic& diagnostic) {
    return diagnostic.location.file + ":" +
           std::to_string(diagnostic.location.line) + ":" +
           std::to_string(diagnostic.location.column) + ": " +
           code_text(diagnostic);
}

std::vector<std::string> diagnostics_of(const fs::path& tree) {
    const Result<Report> report = validate_tree(tree);
    EXPECT_TRUE(report.ok()) << report.status().message();
    std::vector<std::string> got;
    if (!report.ok()) {
        return got;
    }
    for (const Diagnostic& diagnostic : report.value().errors) {
        got.push_back(corpus_line(diagnostic));
    }
    if (passed(report.value())) {
        for (const Diagnostic& diagnostic : report.value().warnings) {
            got.push_back(corpus_line(diagnostic));
        }
    }
    return got;
}

TEST(Corpus, EveryCaseGivesItsExpectedDiagnostics) {
    const std::vector<fs::path> cases = case_directories();
    ASSERT_FALSE(cases.empty());
    for (const fs::path& dir : cases) {
        SCOPED_TRACE(dir.filename().string());
        ASSERT_TRUE(fs::is_directory(dir / "tree")) << "no tree/ in " << dir;
        ASSERT_TRUE(fs::is_regular_file(dir / "expected"))
            << "no expected in " << dir;
        EXPECT_EQ(diagnostics_of(dir / "tree"),
                  expected_lines(dir / "expected"));
    }
}

}  // namespace
}  // namespace scada::conf
