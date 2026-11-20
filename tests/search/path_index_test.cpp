// SPDX-License-Identifier: Apache-2.0
// Indexed ranking is compared with the existing full-scan scorer, never measured by a fabricated time.
#include "frost/search/path_index.hpp"

#include <gtest/gtest.h>
namespace frost::search {
TEST(PathIndex, RankedResultsEqualFullScanAcrossPathAndCamelQueries) {
    std::vector<std::string> paths{"src/FileSystem.cpp", "tests/file_system_test.cpp", "src/main.cpp", "docs/notes.md"};
    for (unsigned i = 0; i < 10000; ++i)
        paths.push_back("generated/Component" + std::to_string(i) + ".cpp");
    PathIndex index;
    ASSERT_TRUE(index.build(paths));
    for (std::string_view query :
         {"FiSy", "src/ma", "cp", "Component9", "nosuch", "", "  ", "/main", "\\main", "\tmain\t", "/", "\\"})
        EXPECT_EQ(index.query(query, paths, 50), rankPaths(query, paths, 50));
}
TEST(PathIndex, CancelledPublicationAndQueriesNeverReturnPartialHits) {
    std::vector<std::string> paths{"src/FileSystem.cpp", "tests/file_system_test.cpp"};
    std::stop_source stopped;
    stopped.request_stop();
    PathIndex index;
    EXPECT_FALSE(index.build(paths, stopped.get_token()));
    EXPECT_TRUE(index.query("Fi", paths, 1).empty());
    ASSERT_TRUE(index.build(paths));
    EXPECT_TRUE(index.query("Fi", paths, 1, stopped.get_token()).empty());
    EXPECT_TRUE(index.query(std::string(257, 'a'), paths, 1).empty());
    EXPECT_TRUE(index.query("Fi", paths, 0).empty());
}
} // namespace frost::search
