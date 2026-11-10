// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for file URIs (uri.hpp): Windows drive and UNC paths, POSIX paths, percent-encoding
// both ways, VS Code's encoded drive colon, the document keys and their file-name prefilter.

#include "frost/lang/uri.hpp"

#include <gtest/gtest.h>

namespace {

using frost::io::PathStyle;
using frost::lang::documentKey;
using frost::lang::mayHaveDocumentKey;
using frost::lang::pathToUri;
using frost::lang::uriToPath;

TEST(Uri, WindowsPaths) {
    EXPECT_EQ(pathToUri("C:\\a b\\x.cpp", PathStyle::Windows).value(), "file:///C:/a%20b/x.cpp");
    EXPECT_EQ(pathToUri("c:\\dir\\#hash%.h", PathStyle::Windows).value(), "file:///C:/dir/%23hash%25.h");
    EXPECT_EQ(pathToUri("\\\\server\\share\\x.c", PathStyle::Windows).value(), "file://server/share/x.c");
    EXPECT_EQ(uriToPath("file:///C:/a%20b/x.cpp", PathStyle::Windows).value(), "C:\\a b\\x.cpp");
    EXPECT_EQ(uriToPath("file:///c%3A/dir/x.cpp", PathStyle::Windows).value(), "C:\\dir\\x.cpp");
    EXPECT_EQ(uriToPath("file://server/share/x.c", PathStyle::Windows).value(), "\\\\server\\share\\x.c");
}

TEST(Uri, PosixPaths) {
    EXPECT_EQ(pathToUri("/home/a b/\xC3\xA9.cpp", PathStyle::Posix).value(), "file:///home/a%20b/%C3%A9.cpp");
    EXPECT_EQ(uriToPath("file:///home/a%20b/%c3%a9.cpp", PathStyle::Posix).value(), "/home/a b/\xC3\xA9.cpp");
}

TEST(Uri, RoundTripsUnusualCharacters) {
    for (const char* path : {"/t/[x]{y}+;=,@!$&'()~_-.cpp", "/t/100%/a?b", "/t/\xF0\x9F\x98\x80"}) {
        auto uri = pathToUri(path, PathStyle::Posix);
        ASSERT_TRUE(uri.has_value()) << path;
        EXPECT_EQ(uriToPath(*uri, PathStyle::Posix).value(), path);
    }
}

TEST(Uri, RejectsWhatIsNotAnAbsoluteFileUri) {
    EXPECT_FALSE(pathToUri("relative/x.cpp", PathStyle::Posix).has_value());
    EXPECT_FALSE(pathToUri("x.cpp", PathStyle::Windows).has_value());
    EXPECT_FALSE(uriToPath("https://example.com/x", PathStyle::Posix).has_value());
    EXPECT_FALSE(uriToPath("file:///a%2", PathStyle::Posix).has_value());
    EXPECT_FALSE(uriToPath("file:///a%zz", PathStyle::Posix).has_value());
}

TEST(Uri, DocumentKeysFoldCaseOnWindowsOnly) {
    EXPECT_EQ(documentKey("C:\\Src\\Main.CPP", PathStyle::Windows), documentKey("c:/src/main.cpp", PathStyle::Windows));
    EXPECT_NE(documentKey("/src/Main.cpp", PathStyle::Posix), documentKey("/src/main.cpp", PathStyle::Posix));
    EXPECT_EQ(documentKey("/src/./main.cpp", PathStyle::Posix), documentKey("/src//main.cpp", PathStyle::Posix));
}

/*
 * The prefilter never rejects a path whose key is the document's (every pair below with equal keys
 * passes), and rejects paths whose file names differ, so callers skip normalising them.
 */
TEST(Uri, TheFileNamePrefilterKeepsEveryPathWithTheKeyAndRejectsOtherNames) {
    struct Case {
        const char* path;
        const char* document;
        PathStyle style;
    };
    const Case same[] = {
        {R"(C:\Src\Main.CPP)", "c:/src/main.cpp", PathStyle::Windows},
        {"C:/src/sub/../main.cpp", R"(C:\src\main.cpp)", PathStyle::Windows},
        {"/src/./main.cpp", "/src//main.cpp", PathStyle::Posix},
        {"/src/main.cpp", "/src/main.cpp", PathStyle::Posix},
        {"C:/src/dir/x/..", R"(C:\src\dir)", PathStyle::Windows}, // POSIX keeps "..": a link may follow.
        {"/src/dir/.", "/src/dir", PathStyle::Posix},
    };
    for (const Case& c : same) {
        const std::string key = documentKey(c.document, c.style);
        ASSERT_EQ(documentKey(c.path, c.style), key) << c.path;
        EXPECT_TRUE(mayHaveDocumentKey(c.path, key)) << c.path;
    }
    EXPECT_FALSE(mayHaveDocumentKey("C:/src/other.cpp", documentKey("C:/src/main.cpp", PathStyle::Windows)));
    EXPECT_FALSE(mayHaveDocumentKey("/src/main.cc", documentKey("/src/main.cpp", PathStyle::Posix)));
    EXPECT_FALSE(mayHaveDocumentKey("/a/main.cpp.in", documentKey("/b/main.cpp", PathStyle::Posix)));
    // Other directories with the same file name pass: only documentKey can tell them apart.
    EXPECT_TRUE(mayHaveDocumentKey("/a/main.cpp", documentKey("/b/main.cpp", PathStyle::Posix)));
}

} // namespace
