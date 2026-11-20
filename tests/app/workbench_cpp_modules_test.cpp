// SPDX-License-Identifier: Apache-2.0
// Deferred CPP-15 normal Workbench action journey with real scanner output and dependency receipts.
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QDialog>
#include <QLabel>
#include <QTableWidget>

#include <chrono>
namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
namespace io = frost::io;
namespace lang = frost::lang;
enum class Mutation { None, EscapingHeader, CompilationDatabase };
void moduleActionJourney(Mutation mutation) {
    frost::test::disableCrashDialogs();
    frost::app::test::WorkDir dir("cpp15-action");
    auto& fs = io::realFileSystem();
    const auto root = dir.join("project");
    const auto module = dir.join("project/unrelated-name.cppm");
    const auto consumer = dir.join("project/consumer.cpp");
    const auto header = dir.join("shared/switch.hpp");
    // The header escapes the project and all -I roots: tracked real preprocessing must find it.
    dir.write("shared/switch.hpp", "#define FEATURE_ENABLED 1\n");
    dir.write("project/unrelated-name.cppm",
              "module;\n#include \"../shared/switch.hpp\"\n"
              "export module action_fixture;\nexport int answer() { return FEATURE_ENABLED; }\n");
    dir.write("project/consumer.cpp", "import action_fixture;\nint consume() { return answer(); }\n");
    const auto compilerDir = std::string(io::parentPath(FROST_TEST_CXX_COMPILER));
    const auto compiler = io::joinPath(compilerDir,
#ifdef _WIN32
                                       "clang++.exe"
#else
                                       "clang++"
#endif
                                       )
                              .value();
    const std::vector<lang::CompileCommand> commands{
        {root, module, {compiler, "-std=c++20", "-c", module, "-o", dir.join("project/interface.o")}},
        {root, consumer, {compiler, "-std=c++20", "-c", consumer, "-o", dir.join("project/consumer.o")}}};
    ASSERT_TRUE(lang::writeCompileCommands(fs, dir.join("project/build"), commands));
    frost::app::test::RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    frost::app::test::WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(root));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(root));
    ASSERT_TRUE(rig.workbench->openPath(consumer));
    ASSERT_TRUE(waitFor(
        [&] {
            auto* service = rig.workbench->clangd();
            return service && service->state() == lang::ClangdState::Running && service->compileCommands();
        },
        90s));
    const auto action = rig.workbench->registry().find("cpp.index.modules");
    ASSERT_TRUE(action);
    EXPECT_EQ(rig.workbench->registry().tryInvoke(*action, {}), frost::input::InvokeResult::Ran);
    QTableWidget* relationships = nullptr;
    ASSERT_TRUE(waitFor(
        [&] {
            relationships = rig.window->findChild<QTableWidget*>(QStringLiteral("cppModuleRelationships"));
            return relationships != nullptr;
        },
        120s));
    int importRow = -1;
    for (int row = 0; row < relationships->rowCount(); ++row)
        if (relationships->item(row, 0)->text().contains(QStringLiteral(" -> action_fixture")) &&
            relationships->item(row, 1)->text().toStdString() == module)
            importRow = row;
    ASSERT_GE(importRow, 0) << "The actual scanner import must resolve to its differently named interface";
    if (mutation == Mutation::EscapingHeader)
        ASSERT_TRUE(fs.writeFileAtomic(header, "#define FEATURE_ENABLED 2\n"));
    if (mutation == Mutation::CompilationDatabase) {
        const auto database = rig.workbench->clangd()->compileCommands();
        ASSERT_TRUE(database);
        auto bytes = fs.readFile(database->file);
        ASSERT_TRUE(bytes);
        ASSERT_TRUE(fs.writeFileAtomic(database->file, *bytes + "\n"));
    }
    emit relationships->cellDoubleClicked(importRow, 0);
    if (mutation == Mutation::None) {
        ASSERT_TRUE(waitFor(
            [&] {
                auto* active = rig.window->tabs().currentEditor();
                return active && active->path() == module;
            },
            30s));
    } else {
        ASSERT_TRUE(waitFor(
            [&] {
                auto* status = rig.window->findChild<QLabel*>(QStringLiteral("cppModuleNavigationStatus"));
                return status && !status->text().isEmpty();
            },
            30s));
        ASSERT_NE(rig.window->tabs().currentEditor(), nullptr);
        EXPECT_EQ(rig.window->tabs().currentEditor()->path(), consumer);
    }
}
TEST(WorkbenchCppModules, NormalActionUsesRealImportRelationshipForInterfaceNavigation) {
    moduleActionJourney(Mutation::None);
}
TEST(WorkbenchCppModules, EscapingDependencyHeaderChangeRefusesSnapshotNavigation) {
    moduleActionJourney(Mutation::EscapingHeader);
}
TEST(WorkbenchCppModules, OriginalDatabaseChangeRefusesSnapshotNavigation) {
    moduleActionJourney(Mutation::CompilationDatabase);
}
} // namespace
