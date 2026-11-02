// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Solution Explorer (frost/ui/toolwindows/solution_explorer.hpp and
// solution_tree_model.hpp): the tree of the tiny and SDK fixture solutions from .sln and .slnx,
// icons, speed search, file activation, the context menu's build and folder commands, and the
// file-manager launch spec. Offscreen; no window is shown.
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/file_manager.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QLineEdit>
#include <QMenu>
#include <QTreeView>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

using project::NodeId;
using project::NodeKind;
using test::FakeBuildBackend;
using test::loadSdk;
using test::nodeAt;
using test::pressKey;
using test::runPostedEvents;
using test::typeText;

// tests/build/fixtures/tiny_sln/<file>, normalised.
std::string tiny(std::string_view file) {
    auto path = io::normalizePath(std::string(FROST_BUILD_FIXTURE_DIR) + "/tiny_sln/" + std::string(file));
    EXPECT_TRUE(path.has_value());
    return path ? *path : std::string();
}

std::shared_ptr<const project::SolutionModel> loadTiny(std::string_view file) {
    auto model = project::SolutionModel::load(io::realFileSystem(), tiny(file));
    EXPECT_TRUE(model.has_value()) << (model ? std::string() : model.error().describe());
    return model ? std::make_shared<const project::SolutionModel>(std::move(*model)) : nullptr;
}

// "Name" for a leaf, "Name[child,child]" otherwise, read through the Qt model.
QString dump(const QAbstractItemModel& model, const QModelIndex& index) {
    QString out = index.data().toString();
    const int rows = model.rowCount(index);
    if (rows > 0) {
        out += u'[';
        for (int row = 0; row < rows; ++row) {
            out += (row > 0 ? QStringLiteral(",") : QString()) + dump(model, model.index(row, 0, index));
        }
        out += u']';
    }
    return out;
}

QString dumpAll(const QAbstractItemModel& model) {
    EXPECT_EQ(model.rowCount(), 1); // The solution is the one top-level row.
    return model.rowCount() == 1 ? dump(model, model.index(0, 0)) : QString();
}

QAction* findAction(QMenu& menu, const char* name) {
    for (QAction* action : menu.actions()) {
        if (action->objectName() == QLatin1String(name)) {
            return action;
        }
    }
    return nullptr;
}

TEST(SolutionTreeModel, ShowsTheTinySolutionFromSlnAndSlnxSorted) {
    for (const std::string_view file : {"Tiny.sln", "Tiny.slnx"}) {
        SCOPED_TRACE(std::string(file));
        SolutionTreeModel model;
        model.setSolution(loadTiny(file));
        // Lib comes first in Tiny.sln; the tree sorts projects by name.
        EXPECT_EQ(dumpAll(model), QStringLiteral("Tiny[App[main.cpp],Lib[lib.cpp,lib.h]]"));
        const QModelIndex solution = model.index(0, 0);
        EXPECT_EQ(solution.data(SolutionTreeModel::KindRole).toInt(), static_cast<int>(NodeKind::Solution));
        EXPECT_EQ(solution.data(SolutionTreeModel::SecondaryTextRole).toString(), QStringLiteral("2 projects"));
        const QModelIndex lib = model.index(1, 0, solution);
        EXPECT_EQ(lib.data(SolutionTreeModel::KindRole).toInt(), static_cast<int>(NodeKind::Project));
        const QModelIndex header = model.index(1, 0, lib);
        EXPECT_EQ(header.data(SolutionTreeModel::PathRole).toString().toStdString(), tiny("Lib/lib.h"));
        EXPECT_EQ(header.parent(), lib);
        EXPECT_EQ(model.indexOfPath(tiny("Lib/lib.h")), header);
    }
}

TEST(SolutionTreeModel, ColoursFileNamesFromTheProvider) {
    SolutionTreeModel model;
    model.setSolution(loadTiny("Tiny.sln"));
    const QModelIndex header = model.indexOfPath(tiny("Lib/lib.h"));
    const QModelIndex source = model.indexOfPath(tiny("Lib/lib.cpp"));
    ASSERT_TRUE(header.isValid());
    ASSERT_TRUE(source.isValid());
    EXPECT_FALSE(header.data(Qt::ForegroundRole).isValid()); // No provider: the default colour.

    // The VCS colours (M5): one colour for lib.h, none for the rest; projects are never coloured.
    int changed = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&changed](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                         changed += roles.contains(Qt::ForegroundRole) ? 1 : 0;
                     });
    const std::string headerPath = tiny("Lib/lib.h");
    model.setFileColorProvider([headerPath](std::string_view path) -> std::optional<QColor> {
        return path == headerPath ? std::optional<QColor>(QColor(0x6F, 0xBF, 0x73)) : std::nullopt;
    });
    EXPECT_GT(changed, 0);
    EXPECT_EQ(header.data(Qt::ForegroundRole).value<QColor>(), QColor(0x6F, 0xBF, 0x73));
    EXPECT_FALSE(source.data(Qt::ForegroundRole).isValid());
    EXPECT_FALSE(header.parent().data(Qt::ForegroundRole).isValid());

    changed = 0;
    model.refreshFileColors();
    EXPECT_GT(changed, 0);
    model.setFileColorProvider({});
    EXPECT_FALSE(header.data(Qt::ForegroundRole).isValid());
}

TEST(SolutionTreeModel, ShowsFoldersFiltersAndLoadErrorsOfTheSdkSolution) {
    for (const std::string_view extension : {"sln", "slnx"}) {
        SCOPED_TRACE(std::string(extension));
        SolutionTreeModel model;
        model.setSolution(loadSdk(extension));
        ASSERT_TRUE(model.solution());
        // Solution folders and filters first, then projects, then files, each by name.
        EXPECT_EQ(
            dumpAll(model),
            QStringLiteral("SdkApp[Libraries[Core[Empty Filter,Generated[Config[config.h]],Header Files[Shared["
                           "common.h],core.h],Source Files[Detail[strings.cpp],core.cpp],core.natvis,gen.py,"
                           "version.h],Engine[engine.cpp,engine.h]],Tools[Gone,Packager,README.md],App[Header "
                           "Files[app.h],Resource Files[app.ico,app.rc],Source Files[app.cpp,main.cpp],README.md]]"));

        const project::SolutionModel& solution = *model.solution();
        const NodeId gone = nodeAt(solution, {"Tools", "Gone"});
        const QModelIndex goneIndex = model.indexOf(gone);
        EXPECT_EQ(goneIndex.data(SolutionTreeModel::SecondaryTextRole).toString(), QStringLiteral("(unavailable)"));
        EXPECT_FALSE(goneIndex.data(Qt::ToolTipRole).toString().isEmpty()); // The load error.
        EXPECT_EQ(model.indexOf(nodeAt(solution, {"Tools", "Packager"})).data(SolutionTreeModel::SecondaryTextRole),
                  QStringLiteral("(not supported yet)"));

        // Icons by kind, from the theme atlas.
        EXPECT_EQ(solutionNodeIconId(solution, project::SolutionModel::root()), icons::kToolwindowStructure);
        EXPECT_EQ(solutionNodeIconId(solution, nodeAt(solution, {"Libraries"})), icons::kNodesFolder);
        EXPECT_EQ(solutionNodeIconId(solution, nodeAt(solution, {"Libraries", "Core"})), icons::kBuildPackage);
        EXPECT_EQ(solutionNodeIconId(solution, nodeAt(solution, {"Libraries", "Core", "Source Files"})),
                  icons::kNodesFolder);
        EXPECT_EQ(solutionNodeIconId(solution, nodeAt(solution, {"Libraries", "Core", "Source Files", "core.cpp"})),
                  icons::kNodesFileCode);
        EXPECT_EQ(solutionNodeIconId(solution, nodeAt(solution, {"Libraries", "Core", "core.natvis"})),
                  icons::kNodesFile);
        EXPECT_EQ(solutionNodeIconId(solution, gone), icons::kStatusFailed);
        EXPECT_EQ(solutionNodeIconId(solution, nodeAt(solution, {"Tools", "Packager"})), icons::kStatusWarning);

        // Every node maps to an index and back.
        for (const NodeId id : model.displayOrder()) {
            EXPECT_EQ(model.nodeOf(model.indexOf(id)), id);
        }
    }
}

TEST(SolutionExplorer, SpeedSearchFindsAFileAndEnterOpensIt) {
    SolutionExplorer explorer;
    explorer.setSolution(loadSdk());
    test::SignalLog opened(&explorer, &SolutionExplorer::openFile);

    typeText(explorer.view(), QStringLiteral("strin"));
    EXPECT_TRUE(explorer.isSpeedSearchActive());
    EXPECT_EQ(explorer.speedSearchText(), QStringLiteral("strin"));
    EXPECT_EQ(explorer.speedSearchField()->text(), QStringLiteral("strin"));
    ASSERT_EQ(explorer.speedSearchMatches().size(), 1U);
    EXPECT_EQ(explorer.currentPath().toStdString(), project::test::sdk("Libraries/Core/src/detail/strings.cpp"));
    // The filter keeps the match and its ancestors only.
    EXPECT_EQ(dumpAll(*explorer.filterModel()),
              QStringLiteral("SdkApp[Libraries[Core[Source Files[Detail[strings.cpp]]]]]"));

    pressKey(explorer.view(), Qt::Key_Return);
    EXPECT_FALSE(explorer.isSpeedSearchActive());
    ASSERT_EQ(opened.count(), 1);
    EXPECT_EQ(opened.at(0).at(0).toString().toStdString(), project::test::sdk("Libraries/Core/src/detail/strings.cpp"));
    // The tree is whole again, with the match still selected.
    EXPECT_EQ(explorer.filterModel()->rowCount(explorer.filterModel()->index(0, 0)), 3);
    EXPECT_EQ(explorer.currentPath().toStdString(), project::test::sdk("Libraries/Core/src/detail/strings.cpp"));
}

TEST(SolutionExplorer, SpeedSearchStepsNarrowsAndEnds) {
    SolutionExplorer explorer;
    explorer.setSolution(loadSdk());
    typeText(explorer.view(), QStringLiteral("readme"));
    ASSERT_EQ(explorer.speedSearchMatches().size(), 2U); // The solution item and App's item (one file).
    const std::optional<NodeId> first = explorer.currentNode();
    ASSERT_TRUE(first.has_value());
    pressKey(explorer.view(), Qt::Key_Down);
    EXPECT_NE(explorer.currentNode(), first);
    pressKey(explorer.view(), Qt::Key_Down); // Wraps around.
    EXPECT_EQ(explorer.currentNode(), first);

    // A query without matches keeps the last matches.
    typeText(explorer.view(), QStringLiteral("zz"));
    EXPECT_EQ(explorer.speedSearchText(), QStringLiteral("readmezz"));
    EXPECT_EQ(explorer.speedSearchMatches().size(), 2U);
    pressKey(explorer.view(), Qt::Key_Backspace);
    pressKey(explorer.view(), Qt::Key_Backspace);
    EXPECT_EQ(explorer.speedSearchText(), QStringLiteral("readme"));

    // A modifier press does not end it; Escape does, without opening anything.
    test::SignalLog opened(&explorer, &SolutionExplorer::openFile);
    pressKey(explorer.view(), Qt::Key_Shift, {}, Qt::ShiftModifier);
    EXPECT_TRUE(explorer.isSpeedSearchActive());
    pressKey(explorer.view(), Qt::Key_Escape);
    EXPECT_FALSE(explorer.isSpeedSearchActive());
    EXPECT_TRUE(explorer.speedSearchField()->isHidden());
    EXPECT_EQ(opened.count(), 0);
    EXPECT_EQ(explorer.currentNode(), first);
}

TEST(SolutionExplorer, DoubleClickOpensFilesAndSelectPathReveals) {
    SolutionExplorer explorer;
    explorer.setSolution(loadSdk());
    test::SignalLog opened(&explorer, &SolutionExplorer::openFile);

    const std::string header = project::test::sdk("App/include/app.h");
    ASSERT_TRUE(explorer.selectPath(header));
    EXPECT_EQ(explorer.currentPath().toStdString(), header);
    EXPECT_TRUE(explorer.view()->isExpanded(explorer.view()->currentIndex().parent())); // Revealed.
    EXPECT_FALSE(explorer.selectPath(project::test::sdk("not-in-the-solution.cpp")));

    emit explorer.view()->doubleClicked(explorer.view()->currentIndex());
    ASSERT_EQ(opened.count(), 1);
    EXPECT_EQ(opened.at(0).at(0).toString().toStdString(), header);
    // Folders and projects do not open.
    emit explorer.view()->doubleClicked(explorer.view()->currentIndex().parent());
    EXPECT_EQ(opened.count(), 1);
}

TEST(SolutionExplorer, ReloadKeepsExpansionAndSelection) {
    SolutionExplorer explorer;
    explorer.setSolution(loadSdk());
    const std::string cpp = project::test::sdk("Libraries/Engine/src/engine.cpp");
    ASSERT_TRUE(explorer.selectPath(cpp));
    explorer.setSolution(loadSdk()); // The same file, loaded again (solution changed on disk).
    EXPECT_EQ(explorer.currentPath().toStdString(), cpp);
    EXPECT_TRUE(explorer.view()->isExpanded(explorer.view()->currentIndex().parent()));
}

TEST(SolutionExplorer, ContextMenuBuildsTheProjectThroughTheController) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    ASSERT_TRUE(controller.openSolution(loadSdk()).has_value());
    SolutionExplorer explorer;
    explorer.setController(&controller);
    ASSERT_EQ(explorer.solution(), controller.solution());

    const project::SolutionModel& solution = *controller.solution();
    const NodeId core = nodeAt(solution, {"Libraries", "Core"});
    const QModelIndex coreIndex = explorer.filterModel()->mapFromSource(explorer.model()->indexOf(core));
    std::unique_ptr<QMenu> menu(explorer.createContextMenu(coreIndex));
    QAction* build = findAction(*menu, "buildProject");
    QAction* rebuild = findAction(*menu, "rebuildProject");
    ASSERT_NE(build, nullptr);
    ASSERT_NE(rebuild, nullptr);
    ASSERT_NE(findAction(*menu, "cleanProject"), nullptr);
    EXPECT_TRUE(build->isEnabled());
    build->trigger();
    rebuild->trigger();
    const auto coreProject = solution.node(core).project;
    ASSERT_TRUE(coreProject.has_value());
    EXPECT_EQ(backend.builds, (std::vector<FakeBuildBackend::BuildCall>{{build::BuildAction::Build, coreProject},
                                                                        {build::BuildAction::Rebuild, coreProject}}));

    // The solution node builds the whole solution.
    std::unique_ptr<QMenu> solutionMenu(explorer.createContextMenu(explorer.filterModel()->index(0, 0)));
    QAction* cleanSolution = findAction(*solutionMenu, "cleanSolution");
    ASSERT_NE(cleanSolution, nullptr);
    cleanSolution->trigger();
    ASSERT_EQ(backend.builds.size(), 3U);
    EXPECT_EQ(backend.builds[2], (FakeBuildBackend::BuildCall{build::BuildAction::Clean, std::nullopt}));

    // An unloaded project cannot be built; nor can anything during a build.
    const NodeId gone = nodeAt(solution, {"Tools", "Gone"});
    std::unique_ptr<QMenu> goneMenu(
        explorer.createContextMenu(explorer.filterModel()->mapFromSource(explorer.model()->indexOf(gone))));
    ASSERT_NE(findAction(*goneMenu, "buildProject"), nullptr);
    EXPECT_FALSE(findAction(*goneMenu, "buildProject")->isEnabled());
    controller.serviceCallbacks().onStateChanged(build::BuildState::Running);
    runPostedEvents();
    std::unique_ptr<QMenu> busyMenu(explorer.createContextMenu(coreIndex));
    EXPECT_FALSE(findAction(*busyMenu, "buildProject")->isEnabled());
}

TEST(SolutionExplorer, ContextMenuOpensTheContainingFolderAndCopiesThePath) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect(io::ScriptedProcess{});
    SolutionExplorer explorer;
    explorer.setSolution(loadSdk());
    explorer.setProcessLauncher(&launcher);
    const std::string cpp = project::test::sdk("Libraries/Core/src/core.cpp");
    const QModelIndex index = explorer.filterModel()->mapFromSource(explorer.model()->indexOfPath(cpp));
    std::unique_ptr<QMenu> menu(explorer.createContextMenu(index));

    QAction* reveal = findAction(*menu, "openContainingFolder");
    ASSERT_NE(reveal, nullptr);
    ASSERT_TRUE(reveal->isEnabled());
    reveal->trigger();
    const std::vector<io::LaunchRecord> launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    auto expected = fileManagerSpec(cpp, false);
    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(launches[0].spec.executable, expected->executable);
    EXPECT_EQ(launches[0].spec.arguments, expected->arguments);

    QAction* copy = findAction(*menu, "copyPath");
    ASSERT_NE(copy, nullptr);
    copy->trigger();
    EXPECT_EQ(QApplication::clipboard()->text().toStdString(), cpp); // Already native.
}

TEST(FileManager, SpecSelectsFilesAndOpensFolders) {
#ifdef _WIN32
    auto file = fileManagerSpec(R"(C:\src\App\main.cpp)", false);
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(io::isAbsolutePath(file->executable));
    EXPECT_TRUE(file->executable.ends_with("explorer.exe"));
    EXPECT_EQ(file->arguments, (std::vector<std::string>{R"(/select,C:\src\App\main.cpp)"}));
    auto folder = fileManagerSpec(R"(C:\src\App)", true);
    ASSERT_TRUE(folder.has_value());
    EXPECT_EQ(folder->arguments, (std::vector<std::string>{R"(C:\src\App)"}));
#else
    auto file = fileManagerSpec("/src/App/main.cpp", false);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->executable, "/usr/bin/xdg-open");
    EXPECT_EQ(file->arguments, (std::vector<std::string>{"/src/App"}));
    auto folder = fileManagerSpec("/src/App", true);
    ASSERT_TRUE(folder.has_value());
    EXPECT_EQ(folder->arguments, (std::vector<std::string>{"/src/App"}));
#endif
    EXPECT_FALSE(file->killOnClose); // The file manager outlives the IDE.
    auto relative = fileManagerSpec("App/main.cpp", false);
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace frost::ui
