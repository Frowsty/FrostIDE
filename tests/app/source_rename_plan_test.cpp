// SPDX-License-Identifier: Apache-2.0
// Proposal assembly and composed Workbench rename/undo tests; execution is deferred.
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "file_transaction_replace_testing.hpp"
#include "workbench_rig.hpp"
#include "workbench_source_rename.hpp"
#include <QScopeGuard>
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
    for (char value : std::string("<Project/>")) {
        bytes += value;
        bytes += '\0';
    }
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
    auto changed = view->editor().document().replace({ByteOffset{0}, ByteOffset{0}}, "// unsaved\n", {});
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

// Real native filesystem/history fixture for application-owned engine boundaries. The guard
// oracle deliberately refuses both the early lease and final decision admissions independently.
TEST(SourceRenameWorkbench, ExternalLeaseAndFinalAdmissionGateHistoryBeforeAnyInverse) {
    app::test::WorkDir dir("external-asset-guard-history");
    dir.write("workspace/asset.lua", "return 1\n");
    dir.write("workspace/asset.lua.fmeta", "stable-handle\n");
    app::test::RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    app::test::WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    auto move =
        lang::planFmetaMove(io::realFileSystem(), dir.join("workspace/asset.lua"), dir.join("workspace/renamed.lua"));
    ASSERT_TRUE(move);
    auto plan = app::prepareSourceFileRename(io::realFileSystem(), *move, "Move engine asset");
    ASSERT_TRUE(plan);
    bool leaseAllowed = false, finalAllowed = false;
    int cancellations = 0;
    std::vector<bool> settled;
    auto external = std::make_shared<document::RefactoringExternalGuard>();
    external->history = [&](bool) -> Result<void> {
        if (!leaseAllowed)
            return fail(ErrorCode::Busy, "actual engine lease refused");
        return {};
    };
    external->admit = [&]() -> Result<void> {
        if (!finalAllowed)
            return fail(ErrorCode::Busy, "actual engine decision receipt refused");
        return {};
    };
    external->cancelled = [&] { ++cancellations; };
    external->settled = [&](bool applied) -> Result<void> {
        settled.push_back(applied);
        return {};
    };
    plan->external = external;
    ASSERT_TRUE(rig.workbench->applyRefactoring(*plan));
    ASSERT_EQ(settled, std::vector<bool>{true});
    EXPECT_FALSE(rig.workbench->undoRefactoring());
    EXPECT_EQ(dir.read("workspace/renamed.lua.fmeta"), "stable-handle\n");
    EXPECT_EQ(cancellations, 0);
    leaseAllowed = true;
    EXPECT_FALSE(rig.workbench->undoRefactoring());
    EXPECT_EQ(cancellations, 1);
    EXPECT_EQ(dir.read("workspace/renamed.lua"), "return 1\n");
    finalAllowed = true;
    ASSERT_TRUE(rig.workbench->undoRefactoring());
    EXPECT_EQ(dir.read("workspace/asset.lua.fmeta"), "stable-handle\n");
    ASSERT_TRUE(rig.workbench->redoRefactoring());
    EXPECT_EQ(settled, (std::vector<bool>{true, false, true}));
}

TEST(SourceRenameWorkbench, SameProcessResumeRetainsExternalLeaseUntilNativeJournalSettles) {
    app::test::WorkDir dir("external-asset-resume");
    dir.write("workspace/Reference.json", "before");
    app::test::RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    app::test::WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    document::RefactoringPlan plan;
    plan.label = "Engine reference edit";
    plan.disk.push_back({dir.join("workspace/Reference.json"), "before", "after"});
    int settled = 0, cancelled = 0;
    plan.external = std::make_shared<document::RefactoringExternalGuard>();
    plan.external->settled = [&](bool applied) -> Result<void> {
        EXPECT_TRUE(applied);
        EXPECT_EQ(dir.read("workspace/Reference.json"), "after");
        ++settled;
        return {};
    };
    plan.external->cancelled = [&] { ++cancelled; };
    bool interrupted = false;
    io::detail::setReplacementObserver(
        [](void* data, io::detail::ReplacementBoundary boundary, const io::FileReplacementReceipt& receipt) {
            if (boundary != io::detail::ReplacementBoundary::OriginalRetired ||
                !receipt.images.targetPath.ends_with("Reference.json") || *static_cast<bool*>(data))
                return;
            *static_cast<bool*>(data) =
                io::realFileSystem().writeFileAtomic(receipt.images.targetPath, "foreign interruption").has_value();
        },
        &interrupted);
    const auto reset = qScopeGuard([] { io::detail::setReplacementObserver(nullptr, nullptr); });
    auto pending = rig.workbench->applyRefactoring(plan);
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending->status, document::RefactoringStatus::PendingRecovery);
    EXPECT_EQ(settled, 0);
    EXPECT_EQ(cancelled, 0);
    io::detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(io::realFileSystem().removeFile(dir.join("workspace/Reference.json")));
    auto resumed = rig.workbench->resumeRefactoring();
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->status, document::RefactoringStatus::Applied);
    EXPECT_EQ(settled, 1);
    EXPECT_EQ(cancelled, 0);
}

} // namespace

TEST(SourceRenamePlan, UnityMetaJoinsOriginalSourceReceiptAndDestinationAbsence) {
    ManualClock clock;
    io::MemFs fs{clock, io::PathStyle::Posix};
    ASSERT_TRUE(fs.createDirectories("/p/Assets"));
    ASSERT_TRUE(fs.writeFileAtomic("/p/Assets/A.cs", "// original\r\n"));
    const std::string metadata = "guid: 0123456789abcdef0123456789abcdef\r\nimporter: unchanged\r\n";
    ASSERT_TRUE(fs.writeFileAtomic("/p/Assets/A.cs.meta", metadata));
    auto move = lang::planFmetaMove(fs, "/p/Assets/A.cs", "/p/Assets/B.cs");
    ASSERT_TRUE(move);
    auto plan = app::prepareSourceFileRename(fs, *move, "Rename Unity asset");
    ASSERT_TRUE(plan);
    ASSERT_TRUE(app::appendUnityMetaRename(fs, *plan, "/p/Assets/A.cs", "/p/Assets/B.cs"));
    ASSERT_EQ(plan->renames.size(), 2);
    EXPECT_EQ(plan->renames[1].expectedBytes, metadata);
    EXPECT_EQ(plan->renames[1].to, "/p/Assets/B.cs.meta");
    const auto target = std::ranges::find(plan->dependencies, "/p/Assets/B.cs.meta", &document::ReadDependency::path);
    ASSERT_NE(target, plan->dependencies.end());
    EXPECT_FALSE(target->expectedBytes);
    EXPECT_EQ(*fs.readFile("/p/Assets/A.cs.meta"), metadata);
    ASSERT_TRUE(fs.writeFileAtomic("/p/Assets/B.cs.meta", "foreign"));
    EXPECT_FALSE(app::appendUnityMetaRename(fs, *plan, "/p/Assets/A.cs", "/p/Assets/B.cs"));
}
