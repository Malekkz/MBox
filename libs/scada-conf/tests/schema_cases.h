#ifndef SCADA_CONF_TESTS_SCHEMA_CASES_H
#define SCADA_CONF_TESTS_SCHEMA_CASES_H

// A schema set of one file, shared by the tests of the schema checks and of
// their cost (schema_limits_test.cpp, a test executable of its own, #164).

#include "scada/common/result.h"
#include "scada/conf/schema.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace scada::conf::schema_cases {

// A set of one schema file, `t.schema.json`, of the text `json`.
inline SchemaSet one_schema(std::string json) {
    std::string problem;
    Result<SchemaSet> set = SchemaSet::compile(
        {SchemaSource{.name = "t.schema.json", .text = std::move(json)}},
        problem);
    EXPECT_TRUE(set.ok()) << problem;
    if (!set.ok()) {
        std::string unused;
        return std::move(SchemaSet::compile({}, unused)).value();
    }
    return std::move(set).value();
}

}  // namespace scada::conf::schema_cases

#endif  // SCADA_CONF_TESTS_SCHEMA_CASES_H
