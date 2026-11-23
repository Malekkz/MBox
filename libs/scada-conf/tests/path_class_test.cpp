#include "scada/conf/tree.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace scada::conf {
namespace {

std::optional<FileType> file_type(std::string_view path) {
    const std::optional<PathClass> found = classify_path(path, false);
    if (!found || found->kind != PathClass::Kind::FILE) {
        return std::nullopt;
    }
    return found->type;
}

bool is_tree_directory(std::string_view path) {
    const std::optional<PathClass> found = classify_path(path, true);
    return found && found->kind == PathClass::Kind::DIRECTORY;
}

// The table of contract section 1.
TEST(PathClass, EveryFileOfTheTable) {
    EXPECT_EQ(file_type("system.yaml"), FileType::SYSTEM);
    EXPECT_EQ(file_type("areas.yaml"), FileType::AREAS);
    EXPECT_EQ(file_type("enums.yaml"), FileType::ENUMS);
    EXPECT_EQ(file_type("logging.yaml"), FileType::LOGGING);
    EXPECT_EQ(file_type("templates/flow_meter_x.yaml"), FileType::TEMPLATE);
    EXPECT_EQ(file_type("drivers/modbus/line1.yaml"), FileType::CHANNEL);
    EXPECT_EQ(file_type("drivers/io/local.yaml"), FileType::CHANNEL);
    EXPECT_EQ(file_type("tags/site.yaml"), FileType::TAGS);
    EXPECT_EQ(file_type("outputs/ps1.yaml"), FileType::OUTPUTS);
    EXPECT_EQ(file_type("alarms/ps1.yaml"), FileType::ALARMS);
    EXPECT_EQ(file_type("control/tasks.yaml"), FileType::TASKS);
    EXPECT_EQ(file_type("control/ps1_level.yaml"), FileType::PROGRAM);
    EXPECT_EQ(file_type("control/ps1_seq.st"), FileType::STRUCTURED_TEXT);
}

TEST(PathClass, ChannelFilesCarryTheirProtocolAndStem) {
    const std::optional<PathClass> found =
        classify_path("drivers/modbus/line1.yaml", false);
    ASSERT_TRUE(found);
    if (found) {
        EXPECT_EQ(found->protocol, "modbus");
        EXPECT_EQ(found->stem, "line1");
    }
}

TEST(PathClass, EveryDirectoryOfTheTable) {
    for (const std::string_view dir :
         {"templates", "drivers", "tags", "outputs", "alarms", "control",
          "drivers/modbus", "drivers/io"}) {
        EXPECT_TRUE(is_tree_directory(dir)) << dir;
    }
}

// Reserved files and directories are refused like unknown ones (CF-101).
TEST(PathClass, ReservedFilesAreNotPartOfTheTree) {
    EXPECT_FALSE(file_type("security.yaml"));
    EXPECT_FALSE(file_type("notifications.yaml"));
    EXPECT_FALSE(file_type("link.yaml"));
    EXPECT_FALSE(is_tree_directory("screens"));
    EXPECT_FALSE(is_tree_directory("symbols"));
    EXPECT_FALSE(is_tree_directory("drivers/link"));
    EXPECT_FALSE(file_type("drivers/link/a.yaml"));
}

TEST(PathClass, PathsOutsideTheGrammarAreRefused) {
    for (const std::string_view path : {"",
                                        "/system.yaml",
                                        "system.yaml/",
                                        "tags//a.yaml",
                                        "./system.yaml",
                                        "../system.yaml",
                                        "tags/../system.yaml",
                                        ".system.yaml",
                                        "tags/.a.yaml",
                                        "tags/a.yaml~",
                                        "tags/a.yaml.swp",
                                        "tags/.a.yaml.swp",
                                        "tags/A.yaml",
                                        "tags/a-b.yaml",
                                        "tags/a b.yaml",
                                        "tags/\xc3\xa9.yaml",
                                        "tags/a.yml",
                                        "tags/a.json",
                                        "tags/.yaml",
                                        "tags/a.YAML",
                                        "tags/a.st",
                                        "templates/a.st",
                                        "system.json",
                                        "System.yaml",
                                        "drivers/modbus/a.st",
                                        "drivers/a.yaml",
                                        "drivers/Modbus/a.yaml",
                                        "drivers/foo/a.yaml",
                                        "drivers/modbus/x/a.yaml",
                                        "control/a/b.yaml",
                                        "a/b/c/d.yaml",
                                        "readme.txt"}) {
        EXPECT_FALSE(classify_path(path, false)) << path;
    }
}

TEST(PathClass, NoSubdirectoriesOutsideDrivers) {
    for (const std::string_view dir :
         {"templates/sub", "tags/sub", "control/sub", "drivers/modbus/sub",
          "drivers/foo", "other", ".git", "Templates", "system.yaml"}) {
        EXPECT_FALSE(is_tree_directory(dir)) << dir;
    }
}

// A directory may not take the name of a file, nor a file that of a
// directory.
TEST(PathClass, FilesAndDirectoriesDoNotSwap) {
    EXPECT_FALSE(file_type("templates"));
    EXPECT_FALSE(is_tree_directory("tags/a.yaml"));
}

TEST(PathClass, StructuredTextHasItsOwnLimit) {
    EXPECT_EQ(file_size_limit(FileType::STRUCTURED_TEXT),
              MAX_STRUCTURED_TEXT_BYTES);
    EXPECT_EQ(file_size_limit(FileType::TEMPLATE), MAX_FILE_BYTES);
}

}  // namespace
}  // namespace scada::conf
