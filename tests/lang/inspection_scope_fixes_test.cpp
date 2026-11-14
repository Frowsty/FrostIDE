// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/inspection_scope_fixes.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
#include <algorithm>
namespace frost::lang {
namespace {
std::string path(std::string_view leaf) { return io::joinPath(io::kNativePathStyle == io::PathStyle::Windows ? "C:/scope" : "/scope", leaf).value(); }
Json replacement(const std::string& file, std::uint64_t offset, std::uint64_t length, std::string text) {
    return Json::object().set("path", file).set("offset", offset).set("length", length).set("text", std::move(text));
}
Json message(Json edits) { return Json::object().set("message", "tool-supplied diagnostic").set("replacements", std::move(edits)); }
Json diagnostic(Json primary, Json notes = Json::array()) {
    return Json::object().set("name", "modernize-use-nullptr").set("directory", path(""))
        .set("primary", message(std::move(primary))).set("notes", std::move(notes));
}
Json decoded(Json diagnostics) { return Json::object().set("kind", "clang-tidy-fixes").set("schemaVersion", 1).set("diagnostics", std::move(diagnostics)); }
InspectionScopeCapture images() {
    InspectionScopeCapture capture; capture.root = path(""); capture.databasePath = path("compile_commands.json");
    capture.images = {{path("a.cpp"), "int* p=0;", "int* p=0;", path("a.cpp"), false, true},
                      {path("h.hpp"), "int* q=0;", "int* q=0;", path("h.hpp"), true, true}};
    return capture;
}
TEST(InspectionScopeFixes, OneActualMultiFileGroupRetainsClosedAndDirtyHeaderImages) {
    auto body = decoded(Json::array({diagnostic(Json::array({replacement(path("a.cpp"), 7, 1, "nullptr"), replacement(path("h.hpp"), 7, 1, "nullptr")}))}));
    auto plan = planInspectionScopeFixes(body, images(), {}); ASSERT_TRUE(plan); ASSERT_TRUE(plan->ready());
    ASSERT_EQ(plan->files.size(), 2U); EXPECT_EQ(plan->files[0].after, "int* p=nullptr;");
    EXPECT_EQ(plan->files[1].after, "int* q=nullptr;"); EXPECT_TRUE(plan->files[1].buffer);
    EXPECT_EQ(plan->groups.front().replacements, 2U);
}
TEST(InspectionScopeFixes, AlternativesUncapturedTargetsAndOverlapsRefuseWholeScope) {
    const auto primary = Json::array({replacement(path("a.cpp"), 7, 1, "nullptr")});
    auto alternatives = planInspectionScopeFixes(decoded(Json::array({diagnostic(primary,
        Json::array({message(Json::array({replacement(path("a.cpp"), 7, 1, "NULL")}))}))})), images(), {});
    ASSERT_TRUE(alternatives); EXPECT_FALSE(alternatives->ready()); EXPECT_TRUE(alternatives->files.empty());
    auto unknown = planInspectionScopeFixes(decoded(Json::array({diagnostic(primary), diagnostic(Json::array({replacement(path("late.hpp"), 0, 1, "x")}))})), images(), {});
    ASSERT_TRUE(unknown); EXPECT_TRUE(unknown->files.empty()); EXPECT_FALSE(unknown->groups.back().refusal.empty());
    auto overlap = planInspectionScopeFixes(decoded(Json::array({diagnostic(primary), diagnostic(Json::array({replacement(path("a.cpp"), 6, 2, " = nullptr")}))})), images(), {});
    ASSERT_TRUE(overlap); EXPECT_FALSE(overlap->ready()); EXPECT_TRUE(overlap->files.empty());
}
TEST(InspectionScopeFixes, NoteOnlyFixIsActualChoiceAndIdenticalConfigurationDuplicatesAreDeduplicated) {
    const auto edits = Json::array({replacement(path("a.cpp"), 7, 1, "nullptr")});
    auto plan = planInspectionScopeFixes(decoded(Json::array({diagnostic(Json::array(), Json::array({message(edits)})), diagnostic(edits)})), images(), {});
    ASSERT_TRUE(plan); ASSERT_TRUE(plan->ready()); ASSERT_EQ(plan->files.size(), 1U);
    EXPECT_EQ(plan->files.front().after, "int* p=nullptr;");
}
TEST(InspectionScopeFixes, CapturePrecedesAnalysisAndRejectsChangedHeaderOrNewInput) {
    io::MemFs fs; ASSERT_TRUE(fs.createDirectories(path("")));
    ASSERT_TRUE(fs.writeFileAtomic(path("a.cpp"), "#include \"h.hpp\"\nint* p=0;"));
    ASSERT_TRUE(fs.writeFileAtomic(path("h.hpp"), "int* q=0;"));
    std::vector<CompileCommand> commands{{path(""), path("a.cpp"), {"clang++", "-c", path("a.cpp")}}};
    ASSERT_TRUE(fs.writeFileAtomic(path("compile_commands.json"), compileCommandsJson(commands)));
    InspectionRequest request; request.compileCommandsDir = path(""); request.scope.kind = InspectionScopeKind::Project;
    auto capture = captureInspectionScope(fs, request, path(""), std::vector<std::pair<std::string,std::string>>{{path("h.hpp"), "int* dirty=0;"}});
    ASSERT_TRUE(capture) << capture.error().describe();
    const auto header = std::ranges::find(capture->images, path("h.hpp"), &InspectionInputImage::path);
    ASSERT_NE(header, capture->images.end()); EXPECT_EQ(header->diskBytes, "int* q=0;"); EXPECT_EQ(header->contents, "int* dirty=0;");
    ASSERT_TRUE(fs.writeFileAtomic(path("h.hpp"), "int changed;")); EXPECT_FALSE(validateInspectionCapture(fs, *capture));
    ASSERT_TRUE(fs.writeFileAtomic(path("h.hpp"), "int* q=0;")); ASSERT_TRUE(fs.writeFileAtomic(path("new.hpp"), "int fresh;"));
    EXPECT_FALSE(validateInspectionCapture(fs, *capture));
}
TEST(InspectionScopeFixes, AbsoluteSourceDoesNotMakeRelativeWorkingDirectorySafeToCopy) {
    io::MemFs fs; ASSERT_TRUE(fs.createDirectories(path("")));
    ASSERT_TRUE(fs.writeFileAtomic(path("a.cpp"), "int* p=0;"));
    std::vector<CompileCommand> commands{{"relative", path("a.cpp"), {"clang++", "-c", path("a.cpp")}}};
    ASSERT_TRUE(fs.writeFileAtomic(path("compile_commands.json"), compileCommandsJson(commands)));
    InspectionRequest request; request.compileCommandsDir = path(""); request.scope.kind = InspectionScopeKind::Project;
    auto capture = captureInspectionScope(fs, request, path(""), {});
    ASSERT_FALSE(capture); EXPECT_NE(capture.error().describe().find("absolute working directories"), std::string::npos);
}
TEST(InspectionScopeFixes, SelectedResponseFileIsExplicitlyUnsupportedRatherThanReadLive) {
    io::MemFs fs; ASSERT_TRUE(fs.createDirectories(path("build")));
    ASSERT_TRUE(fs.writeFileAtomic(path("a.cpp"), "int* p=0;"));
    ASSERT_TRUE(fs.writeFileAtomic(path("build/args.rsp"), "-DVALUE=1"));
    std::vector<CompileCommand> commands{{path(""), path("a.cpp"), {"clang++", "@" + path("build/args.rsp"), "-c", path("a.cpp")}}};
    ASSERT_TRUE(fs.writeFileAtomic(path("compile_commands.json"), compileCommandsJson(commands)));
    InspectionRequest request; request.compileCommandsDir = path(""); request.scope.kind = InspectionScopeKind::Project;
    auto capture = captureInspectionScope(fs, request, path(""), {});
    ASSERT_FALSE(capture); EXPECT_NE(capture.error().describe().find("response-file"), std::string::npos);
}
TEST(InspectionScopeFixes, ExplicitGeneratedIncludeDirectoryAndExtensionlessForcedHeaderAreCaptured) {
    io::MemFs fs; ASSERT_TRUE(fs.createDirectories(path("build/generated")));
    ASSERT_TRUE(fs.writeFileAtomic(path("a.cpp"), "int* p=0;"));
    ASSERT_TRUE(fs.writeFileAtomic(path("build/generated/config"), "#define VALUE 1"));
    ASSERT_TRUE(fs.writeFileAtomic(path("build/forced"), "#define FORCE 1"));
    std::vector<CompileCommand> commands{{path(""), path("a.cpp"), {"clang++", "-Ibuild/generated", "-include", "build/forced", "-c", path("a.cpp")}}};
    ASSERT_TRUE(fs.writeFileAtomic(path("compile_commands.json"), compileCommandsJson(commands)));
    InspectionRequest request; request.compileCommandsDir = path(""); request.scope.kind = InspectionScopeKind::Project;
    auto capture = captureInspectionScope(fs, request, path(""), {});
    ASSERT_TRUE(capture) << capture.error().describe();
    for (const auto leaf : {"build/generated/config", "build/forced"}) {
        const auto found = std::ranges::find(capture->images, path(leaf), &InspectionInputImage::path);
        ASSERT_NE(found, capture->images.end()); EXPECT_FALSE(found->editable);
    }
    ASSERT_TRUE(fs.writeFileAtomic(path("build/generated/config"), "#define VALUE 2"));
    EXPECT_FALSE(validateInspectionCapture(fs, *capture));
}

TEST(InspectionScopeFixes, ExplicitModuleMapCannotBypassImmutableInputProfile) {
    io::MemFs fs; ASSERT_TRUE(fs.createDirectories(path("build")));
    ASSERT_TRUE(fs.writeFileAtomic(path("a.cpp"), "int* p=0;"));
    ASSERT_TRUE(fs.writeFileAtomic(path("build/module.modulemap"), "module Example { header \"generated.hpp\" }"));
    std::vector<CompileCommand> commands{{path(""), path("a.cpp"), {"clang++", "-fmodule-map-file=" + path("build/module.modulemap"), "-c", path("a.cpp")}}};
    ASSERT_TRUE(fs.writeFileAtomic(path("compile_commands.json"), compileCommandsJson(commands)));
    InspectionRequest request; request.compileCommandsDir = path(""); request.scope.kind = InspectionScopeKind::Project;
    auto capture = captureInspectionScope(fs, request, path(""), {});
    ASSERT_FALSE(capture); EXPECT_NE(capture.error().describe().find("module"), std::string::npos);
}

}
}
