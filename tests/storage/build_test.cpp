// SPDX-License-Identifier: Apache-2.0
//
// The linked SQLite and xxHash are the pinned builds (ADR 0014), and the run-time build check of
// sqlite_init.cpp accepts them.

#include "sqlite_init.hpp"
#include <gtest/gtest.h>

#include <string_view>

#include <sqlite3.h>
#include <xxhash.h>

namespace {

TEST(StorageBuild, LinkedLibraryMatchesPin) {
    EXPECT_EQ(std::string_view(sqlite3_libversion()), "3.54.1");
    EXPECT_EQ(sqlite3_libversion_number(), frost::storage::detail::kPinnedSqliteVersionNumber);
    EXPECT_NE(sqlite3_threadsafe(), 0);
    EXPECT_EQ(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION"), 1);
    EXPECT_EQ(sqlite3_compileoption_used("OMIT_JSON"), 1);
    EXPECT_EQ(XXH_versionNumber(), 804u);
    auto checked = frost::storage::detail::checkLinkedSqlite();
    EXPECT_TRUE(checked.has_value()) << checked.error().describe();
    auto initialized = frost::storage::detail::initializeSqlite();
    EXPECT_TRUE(initialized.has_value()) << initialized.error().describe();
    // Idempotent.
    EXPECT_TRUE(frost::storage::detail::initializeSqlite().has_value());
}

} // namespace
