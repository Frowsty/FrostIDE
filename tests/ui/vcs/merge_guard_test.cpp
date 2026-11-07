// SPDX-License-Identifier: Apache-2.0
//
// Tests of the 3-way merge dialog's Apply guard (frost/ui/vcs/merge_guard.hpp, FND-3): Apply
// refuses while chunks are unresolved or the result still holds git conflict markers, and allows a
// clean result, including text whose "=======" lines are not part of a conflict.

#include "frost/ui/vcs/merge_guard.hpp"

#include <gtest/gtest.h>

namespace {

using frost::ui::hasConflictMarkers;
using frost::ui::mergeApplyBlocker;

TEST(MergeGuard, DetectsGitConflictMarkerLines) {
    EXPECT_TRUE(hasConflictMarkers("a\n<<<<<<< HEAD\nmine\n=======\ntheirs\n>>>>>>> feature\nb\n"));
    EXPECT_TRUE(hasConflictMarkers("<<<<<<<\nx\n"));
    EXPECT_TRUE(hasConflictMarkers("x\r\n||||||| base\r\ny\r\n"));
    EXPECT_TRUE(hasConflictMarkers("x\n>>>>>>>"));
    // Not a marker: a different length, indented, or the separator alone.
    EXPECT_FALSE(hasConflictMarkers("<<<<<<<< eight\n"));
    EXPECT_FALSE(hasConflictMarkers("  <<<<<<< indented\n"));
    EXPECT_FALSE(hasConflictMarkers("std::cout << x;\nTitle\n=======\n"));
    EXPECT_FALSE(hasConflictMarkers("<<<<<<<x\n"));
    EXPECT_FALSE(hasConflictMarkers(""));
}

TEST(MergeGuard, ApplyRefusesUnresolvedChunksAndLeftoverMarkers) {
    const std::string clean = "int main() {\n    return 0;\n}\n";
    EXPECT_EQ(mergeApplyBlocker(0, clean), "");
    EXPECT_EQ(mergeApplyBlocker(0, "Heading\n=======\n"), "");
    const std::string one = mergeApplyBlocker(1, clean);
    EXPECT_NE(one.find("1 conflict "), std::string::npos) << one;
    EXPECT_NE(mergeApplyBlocker(3, clean).find("3 conflicts"), std::string::npos);
    // Every chunk resolved in the model, but a marker was typed back (or left) in the result.
    const std::string markers = mergeApplyBlocker(0, "a\n<<<<<<< HEAD\nb\n>>>>>>> x\n");
    EXPECT_NE(markers.find("conflict markers"), std::string::npos) << markers;
}

} // namespace
