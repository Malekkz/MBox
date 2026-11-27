#ifndef SCADA_TAG_TESTS_TAG_SET_EXPECT_H
#define SCADA_TAG_TESTS_TAG_SET_EXPECT_H

// What `validate` and `config_hash` say of a tag set, shared by the tests of
// the tag set and of its count limits (tag_set_limits_test.cpp, a test
// executable of its own, #164).

#include "scada/common/result.h"
#include "scada/common/status.h"
#include "scada/tag/tag_set.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace scada::tag::tag_set_expect {

// Expects that `tag_set` is rejected with `code` and a detail that contains
// `fragment`.
inline void expect_rejected(const TagSet& tag_set, StatusCode code,
                            std::string_view fragment) {
    std::string detail;
    const Status status = validate(tag_set, detail);
    EXPECT_EQ(status.code(), code) << detail;
    EXPECT_NE(detail.find(fragment), std::string::npos)
        << "detail: " << detail << "\nexpected to contain: " << fragment;
    // The hash refuses the same tag set for the same reason.
    std::string hash_detail;
    const Result<ConfigHash> hash = config_hash(tag_set, hash_detail);
    EXPECT_EQ(hash.status().code(), code);
    EXPECT_EQ(hash_detail, detail);
}

inline void expect_valid(const TagSet& tag_set) {
    std::string detail = "untouched";
    EXPECT_TRUE(validate(tag_set, detail).ok()) << detail;
    EXPECT_EQ(detail, "untouched");
}

}  // namespace scada::tag::tag_set_expect

#endif  // SCADA_TAG_TESTS_TAG_SET_EXPECT_H
