// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost/build/build_diagnostics.hpp on every platform: tool origins and build products,
// path resolution (absolute, relative to the reporting project, relative to the solution), the
// jump decision against a MemFs, the index's counts, groups and per-document query, and the
// "Project.vcxproj -> output" lines.

#include "frost/build/build_diagnostics.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::build {
namespace {

using test::MemTree;

Diagnostic diagnostic(Severity severity, std::string code, std::string file, int line, std::string project = {}) {
    return Diagnostic{.severity = severity,
                      .code = std::move(code),
                      .message = "text",
                      .file = std::move(file),
                      .line = line,
                      .column = line > 0 ? 3 : 0,
                      .project = std::move(project)};
}

TEST(BuildDiagnostics, ToolOriginsAndBuildProducts) {
    for (const std::string_view tool : {"LINK", "MSBUILD", "cl", "EXEC", "lld-link.exe", "CL.EXE"}) {
        EXPECT_TRUE(isToolOrigin(tool)) << tool;
    }
    for (const std::string_view file : {"main.cpp", "main.obj", "src/a.h", "C:\\x\\LINK", ""}) {
        EXPECT_FALSE(isToolOrigin(file)) << file;
    }
    for (const std::string_view product : {"main.obj", "x.O", "Lib.lib", "App.exe", "a.pdb", "r.res", "x.dll"}) {
        EXPECT_TRUE(isBuildProduct(product)) << product;
    }
    for (const std::string_view source : {"main.cpp", "lib.h", "res.rc", "Makefile"}) {
        EXPECT_FALSE(isBuildProduct(source)) << source;
    }
}

TEST(BuildDiagnostics, ResolvesPathsAndDecidesWhatIsJumpable) {
    MemTree tree;
    tree.write("Game/App/main.cpp", "int main() {}\n");
    tree.write("Game/App/local.h", "#pragma once\n");
    tree.write("Game/shared.h", "#pragma once\n");
    const std::string project = MemTree::path("Game/App/App.vcxproj");
    DiagnosticResolver resolver(tree.fs, MemTree::path("Game"));

    // Absolute, as cl prints it with /FC.
    const BuildDiagnostic absolute =
        resolver.resolve(diagnostic(Severity::Error, "C2065", MemTree::path("Game/App/main.cpp"), 7, project));
    EXPECT_EQ(absolute.path, MemTree::path("Game/App/main.cpp"));
    EXPECT_TRUE(absolute.jumpable);

    // Relative: against the reporting project's directory, else the solution's.
    const BuildDiagnostic relative =
        resolver.resolve(diagnostic(Severity::Warning, "C4100", test::native("./local.h"), 2, project));
    EXPECT_EQ(relative.path, MemTree::path("Game/App/local.h"));
    EXPECT_TRUE(relative.jumpable);
    const BuildDiagnostic fallback = resolver.resolve(diagnostic(Severity::Warning, "C4100", "shared.h", 1));
    EXPECT_EQ(fallback.path, MemTree::path("Game/shared.h"));
    EXPECT_TRUE(fallback.jumpable);

    // No line, a missing file, an object file, a tool: never jumpable.
    EXPECT_FALSE(
        resolver.resolve(diagnostic(Severity::Error, "C1083", MemTree::path("Game/App/main.cpp"), 0)).jumpable);
    const BuildDiagnostic missing =
        resolver.resolve(diagnostic(Severity::Error, "C1083", MemTree::path("Game/App/gone.cpp"), 4, project));
    EXPECT_EQ(missing.path, MemTree::path("Game/App/gone.cpp"));
    EXPECT_FALSE(missing.jumpable);
    const BuildDiagnostic object = resolver.resolve(diagnostic(Severity::Error, "LNK2019", "main.obj", 1, project));
    EXPECT_EQ(object.path, MemTree::path("Game/App/main.obj"));
    EXPECT_FALSE(object.jumpable);
    const BuildDiagnostic tool = resolver.resolve(diagnostic(Severity::Error, "LNK1104", "LINK", 0, project));
    EXPECT_TRUE(tool.path.empty());
    EXPECT_FALSE(tool.jumpable);
    const BuildDiagnostic none = resolver.resolve(diagnostic(Severity::Error, "MSB1009", "", 0));
    EXPECT_TRUE(none.path.empty());
    EXPECT_FALSE(none.jumpable);
    EXPECT_EQ(none.diagnostic.code, "MSB1009"); // The parsed diagnostic is kept as it was.
}

TEST(BuildDiagnostics, IndexCountsGroupsAndAnswersPerDocument) {
    MemTree tree;
    tree.write("Game/App/main.cpp", "");
    DiagnosticResolver resolver(tree.fs, MemTree::path("Game"));
    const std::string main = MemTree::path("Game/App/main.cpp");
    DiagnosticIndex index;
    index.add(resolver.resolve(diagnostic(Severity::Error, "C2065", main, 7)));
    index.add(resolver.resolve(diagnostic(Severity::Error, "LNK1104", "LINK", 0)));
    index.add(resolver.resolve(diagnostic(Severity::Warning, "C4996", main, 6)));
    index.add(resolver.resolve(diagnostic(Severity::Message, "", main, 9)));
    index.add(resolver.resolve(diagnostic(Severity::Error, "LNK2001", "LINK", 0)));

    EXPECT_EQ(index.counts(), (DiagnosticCounts{.errors = 3, .warnings = 1, .messages = 1}));
    EXPECT_EQ(index.all().size(), 5U);
    const std::vector<DiagnosticFileGroup> groups = index.byFile();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(groups[0].file, main);
    EXPECT_TRUE(groups[0].isPath);
    EXPECT_EQ(groups[0].diagnostics.size(), 3U);
    EXPECT_EQ(groups[0].counts, (DiagnosticCounts{.errors = 1, .warnings = 1, .messages = 1}));
    EXPECT_EQ(groups[1].file, "LINK");
    EXPECT_FALSE(groups[1].isPath);
    EXPECT_EQ(groups[1].counts.errors, 2U);

    const std::vector<BuildDiagnostic> forMain = index.forDocument(main);
    ASSERT_EQ(forMain.size(), 3U);
    EXPECT_EQ(forMain[0].diagnostic.code, "C2065");
    EXPECT_EQ(forMain[1].diagnostic.code, "C4996");
    if constexpr (test::kWindows) {
        // Windows paths compare with ASCII case ignored.
        EXPECT_EQ(index.forDocument("c:\\WORK\\game\\app\\MAIN.CPP").size(), 3U);
    }
    EXPECT_TRUE(index.forDocument(MemTree::path("Game/other.cpp")).empty());
    EXPECT_TRUE(index.forDocument("main.cpp").empty()); // Relative paths name no document.

    index.clear();
    EXPECT_TRUE(index.empty());
    EXPECT_EQ(index.counts(), DiagnosticCounts{});
    EXPECT_TRUE(index.forDocument(main).empty());
}

TEST(BuildDiagnostics, ReadsProjectOutputLines) {
    EXPECT_EQ(parseProjectOutputLine("  Lib.vcxproj -> C:\\out\\x64\\Debug\\Lib.lib\r"),
              (ProjectOutputLine{.projectFile = "Lib.vcxproj", .output = "C:\\out\\x64\\Debug\\Lib.lib"}));
    EXPECT_EQ(parseProjectOutputLine("3>My Tool.csproj -> /out/tool.dll"),
              (ProjectOutputLine{.projectFile = "My Tool.csproj", .output = "/out/tool.dll"}));
    for (const std::string_view other :
         {"  main.cpp", "C:\\s\\App.vcxproj -> C:\\x.exe", "Lib.vcxproj ->", "a -> b", "Lib.txt -> C:\\x",
          "main.cpp(3): error C2065: x -> y [C:\\App.vcxproj]", ""}) {
        EXPECT_FALSE(parseProjectOutputLine(other).has_value()) << other;
    }
}

} // namespace
} // namespace frost::build
