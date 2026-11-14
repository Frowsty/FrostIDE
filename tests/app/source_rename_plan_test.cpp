// SPDX-License-Identifier: Apache-2.0
// Proposal assembly and composed Workbench rename/undo tests; execution is deferred.
#include "workbench_source_rename.hpp"
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "workbench_rig.hpp"

#include <gtest/gtest.h>
#include <algorithm>

namespace {

using namespace frost;

TEST(SourceRenamePlan, SourceAndSidecarAndMembershipJoinOneUnappliedPlan) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/old.cpp", "source bytes"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/old.cpp.fmeta", "stable-guid"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/CMakeLists.txt", "add_library(x old.cpp)"));
    auto move = lang::planFmetaMove(fs, "/p/old.cpp", "/p/new.cpp");
    ASSERT_TRUE(move);
    auto plan = app::prepareSourceFileRename(fs, *move, "Rename source");
    ASSERT_TRUE(plan);
    auto input = app::captureRefactoringInput(fs, "/p/CMakeLists.txt");
    ASSERT_TRUE(input);
    ASSERT_TRUE(app::appendRefactoringReplacement(*plan, *input, "add_library(x old.cpp)", "add_library(x new.cpp)"));
    ASSERT_EQ(plan->renames.size(), 2);
    EXPECT_EQ(plan->renames[0].expectedBytes, "source bytes");
    EXPECT_EQ(plan->renames[1].expectedBytes, "stable-guid");
    ASSERT_EQ(plan->disk.size(), 1);
    EXPECT_EQ(plan->disk[0].expectedBytes, "add_library(x old.cpp)");
    EXPECT_EQ(plan->disk[0].afterBytes, "add_library(x new.cpp)");
    EXPECT_EQ(*fs.readFile("/p/old.cpp"), "source bytes");
    EXPECT_EQ(*fs.readFile("/p/old.cpp.fmeta"), "stable-guid");
    EXPECT_EQ(*fs.readFile("/p/CMakeLists.txt"), "add_library(x old.cpp)");
    EXPECT_FALSE(fs.exists("/p/new.cpp").value_or(true));
}

TEST(SourceRenamePlan, AbsentSidecarsAreReadDependenciesRatherThanInventedFiles) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/old.cpp", "bytes"));
    auto move = lang::planFmetaMove(fs, "/p/old.cpp", "/p/new.cpp");
    ASSERT_TRUE(move);
    auto plan = app::prepareSourceFileRename(fs, *move, "Rename");
    ASSERT_TRUE(plan);
    ASSERT_EQ(plan->renames.size(), 1);
    for (std::string_view path : {"/p/old.cpp.fmeta", "/p/new.cpp.fmeta"}) {
        auto dependency = std::ranges::find(plan->dependencies, path, &document::ReadDependency::path);
        ASSERT_NE(dependency, plan->dependencies.end());
        EXPECT_FALSE(dependency->expectedBytes.has_value());
    }
}

TEST(SourceRenamePlan, OrphanDestinationMetadataAndChangedSourceSidecarRefuseBeforeEffects) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/old.cpp", "source"));
    auto move = lang::planFmetaMove(fs, "/p/old.cpp", "/p/new.cpp");
    ASSERT_TRUE(move);
    ASSERT_TRUE(fs.writeFileAtomic("/p/new.cpp.fmeta", "unrelated-guid"));
    auto orphan = app::prepareSourceFileRename(fs, *move, "Rename");
    ASSERT_FALSE(orphan);
    EXPECT_EQ(orphan.error().code(), ErrorCode::AlreadyExists);
    ASSERT_TRUE(fs.removeFile("/p/new.cpp.fmeta"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/old.cpp.fmeta", "newly-created-guid"));
    auto changed = app::prepareSourceFileRename(fs, *move, "Rename");
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().code(), ErrorCode::Busy);
    EXPECT_EQ(*fs.readFile("/p/old.cpp"), "source");
    EXPECT_FALSE(fs.exists("/p/new.cpp").value_or(true));
}

TEST(SourceRenamePlan, DirtyMembershipUsesBufferReplacementAndBothReadReceipts) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/Project.vcxproj", "saved old.cpp"));
    auto input = app::captureRefactoringInput(fs, "/p/Project.vcxproj", "unsaved old.cpp");
    ASSERT_TRUE(input);
    document::RefactoringPlan plan;
    ASSERT_TRUE(app::appendRefactoringReplacement(plan, *input, "unsaved old.cpp", "unsaved new.cpp"));
    EXPECT_TRUE(plan.disk.empty());
    ASSERT_EQ(plan.buffers.size(), 1);
    EXPECT_EQ(plan.buffers[0].expectedText, "unsaved old.cpp");
    EXPECT_EQ(plan.buffers[0].afterText, "unsaved new.cpp");
    EXPECT_EQ(plan.buffers[0].persistence, document::BufferPersistence::Automatic);
    ASSERT_EQ(plan.dependencies.size(), 1);
    EXPECT_EQ(plan.dependencies[0].expectedBytes, "saved old.cpp");
    ASSERT_EQ(plan.bufferDependencies.size(), 1);
    EXPECT_EQ(plan.bufferDependencies[0].expectedText, "unsaved old.cpp");
    EXPECT_EQ(*fs.readFile("/p/Project.vcxproj"), "saved old.cpp");
}

TEST(SourceRenamePlan, OpenUtf8BomDefinitionsCompareDecodedBufferWithoutChangingItsFormat) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    const std::string before = "\xef\xbb\xbf<Project>old.cpp</Project>";
    ASSERT_TRUE(fs.writeFileAtomic("/p/Project.vcxproj", before));
    auto input = app::captureRefactoringInput(fs, "/p/Project.vcxproj", "<Project>old.cpp</Project>");
    ASSERT_TRUE(input);
    document::RefactoringPlan plan;
    ASSERT_TRUE(app::appendRefactoringReplacement(plan, *input, before, "\xef\xbb\xbf<Project>new.cpp</Project>"));
    ASSERT_EQ(plan.buffers.size(), 1);
    EXPECT_EQ(plan.buffers[0].expectedText, "<Project>old.cpp</Project>");
    EXPECT_EQ(plan.buffers[0].afterText, "<Project>new.cpp</Project>");
    EXPECT_EQ(*fs.readFile("/p/Project.vcxproj"), before);
}

TEST(SourceRenamePlan, ReadOnlyDirtyInputDependencyDoesNotInventAnEdit) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/Project.vcxproj.filters", "saved"));
    auto input = app::captureRefactoringInput(fs, "/p/Project.vcxproj.filters", "unsaved");
    ASSERT_TRUE(input);
    document::RefactoringPlan plan;
    ASSERT_TRUE(app::appendRefactoringDependency(plan, *input));
    ASSERT_TRUE(app::appendRefactoringDependency(plan, *input));
    EXPECT_TRUE(plan.disk.empty());
    EXPECT_TRUE(plan.buffers.empty());
    EXPECT_EQ(plan.dependencies.size(), 1);
    EXPECT_EQ(plan.bufferDependencies.size(), 1);
    input->buffer = "later mutation";
    auto conflict = app::appendRefactoringDependency(plan, *input);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code(), ErrorCode::Busy);
}

TEST(SourceRenamePlan, MissingAndNonRegularInputsStayDistinctAndNoPartialMoveOccurs) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/old.cpp", "source"));
    ASSERT_TRUE(fs.createDirectories("/p/directory"));
    auto missing = app::captureRefactoringInput(fs, "/p/missing.filters");
    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing->disk);
    EXPECT_FALSE(app::captureRefactoringInput(fs, "/p/directory"));
    auto move = lang::planFmetaMove(fs, "/p/old.cpp", "/p/new.cpp");
    ASSERT_TRUE(move);
    auto plan = app::prepareSourceFileRename(fs, *move, "Rename");
    ASSERT_TRUE(plan);
    ASSERT_TRUE(fs.writeFileAtomic("/p/CMakeLists.txt", "actual"));
    auto input = app::captureRefactoringInput(fs, "/p/CMakeLists.txt");
    ASSERT_TRUE(input);
    EXPECT_FALSE(app::appendRefactoringReplacement(*plan, *input, "stale", "replacement"));
    EXPECT_EQ(*fs.readFile("/p/old.cpp"), "source");
    EXPECT_FALSE(fs.exists("/p/new.cpp").value_or(true));
}

TEST(SourceRenamePlan, DirtyImportedPropsCannotBorrowTheHelpersMatchingDiskReceipt) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/shared.props", "<ClInclude Include='old.hpp'/>"));
    auto input = app::captureRefactoringInput(fs, "/p/shared.props", "<ClInclude Include='other.hpp'/>");
    ASSERT_TRUE(input);
    // ProjectHost evaluated the disk bytes. Leasing this unrelated dirty overlay would not make
    // it the evaluated project: the rename must refuse before assembling any mutable effects.
    auto matched = app::refactoringInputMatchesEvaluation(*input, input->disk);
    ASSERT_FALSE(matched);
    EXPECT_EQ(matched.error().code(), ErrorCode::Busy);
    EXPECT_EQ(*fs.readFile("/p/shared.props"), "<ClInclude Include='old.hpp'/>");
}

TEST(SourceRenamePlan, CleanUtf16ImportedBufferMatchesOnlyItsLosslesslyDecodedEvaluation) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p"));
    std::string bytes = "\xff\xfe";
    for (char value : std::string("<Project/>")) { bytes += value; bytes += '\0'; }
    ASSERT_TRUE(fs.writeFileAtomic("/p/shared.props", bytes));
    auto input = app::captureRefactoringInput(fs, "/p/shared.props", "<Project/>");
    ASSERT_TRUE(input);
    EXPECT_TRUE(app::refactoringInputMatchesEvaluation(*input, input->disk));
    input->buffer = "<Project><ClInclude Include='other.hpp'/></Project>";
    EXPECT_FALSE(app::refactoringInputMatchesEvaluation(*input, input->disk));
}

TEST(SourceRenameWorkbench, DirtyOpenSourceAndMetadataRenameAndUndoAsOneOperation) {
    app::test::WorkDir dir("file-rename-transaction");
    dir.write("workspace/old.cpp", "int original;\n");
    dir.write("workspace/old.cpp.fmeta", "stable-guid\n");
    app::test::RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    app::test::WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace/old.cpp")));
    auto* view = rig.view();
    ASSERT_NE(view, nullptr);
    auto changed = view->editor().document().replace({0, 0}, "// unsaved\n", {});
    ASSERT_TRUE(changed);
    ASSERT_TRUE(view->editor().followExternalChange(*changed));
    ASSERT_TRUE(view->document().dirty());
    const auto dirtyText = view->editor().document().text().text();
    auto renamed = rig.workbench->renameItem(dir.join("workspace/old.cpp"), "new.cpp");
    ASSERT_TRUE(renamed) << renamed.error().message();
    EXPECT_EQ(rig.workbench->editors().find(dir.join("workspace/new.cpp")), view);
    EXPECT_EQ(view->document().path(), dir.join("workspace/new.cpp"));
    EXPECT_TRUE(view->document().dirty());
    EXPECT_EQ(view->editor().document().text().text(), dirtyText);
    EXPECT_EQ(dir.read("workspace/new.cpp"), "int original;\n");
    EXPECT_EQ(dir.read("workspace/new.cpp.fmeta"), "stable-guid\n");
    auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone->status, document::RefactoringStatus::Undone);
    EXPECT_EQ(rig.workbench->editors().find(dir.join("workspace/old.cpp")), view);
    EXPECT_EQ(view->document().path(), dir.join("workspace/old.cpp"));
    EXPECT_TRUE(view->document().dirty());
    EXPECT_EQ(view->editor().document().text().text(), dirtyText);
    EXPECT_EQ(dir.read("workspace/old.cpp"), "int original;\n");
    EXPECT_EQ(dir.read("workspace/old.cpp.fmeta"), "stable-guid\n");
    auto redone = rig.workbench->redoRefactoring();
    ASSERT_TRUE(redone);
    EXPECT_EQ(redone->status, document::RefactoringStatus::Redone);
    EXPECT_EQ(view->document().path(), dir.join("workspace/new.cpp"));
    EXPECT_EQ(dir.read("workspace/new.cpp.fmeta"), "stable-guid\n");
}

} // namespace
