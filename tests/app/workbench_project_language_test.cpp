// SPDX-License-Identifier: Apache-2.0
// Deferred R16 normal editor actions, actual configure provenance, atomic apply and stale-preview refusal.
#include "frost/build/cmake_service.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/msbuild_language.hpp"
#include "frost/ui/lang/lang_widgets.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "workbench_rig.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>
#include <QTimer>

#include <algorithm>
#include <chrono>
namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
TEST(WorkbenchProjectLanguage, UntrustedPropsNavigationUsesLiteralSourceImportWithoutEvaluation) {
    WorkDir dir("sln18-source-nav");
    dir.write("project/imported.props",
              "<Project><PropertyGroup><ImportedValue>source</ImportedValue></PropertyGroup></Project>\n");
    const std::string source = "<Project>\n<Import Project=\"imported.props\" "
                               "/>\n<PropertyGroup><Use>$(ImportedValue)</Use></PropertyGroup>\n</Project>\n";
    dir.write("project/main.targets", source);
    frost::io::ScriptedProcessLauncher launcher;
    frost::app::test::RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project/main.targets")));
    auto* view = rig.view();
    ASSERT_NE(view, nullptr);
    const auto at = source.find("$(ImportedValue)") + 3;
    view->editor().editor().setState(frost::text::EditorState::caretAt(frost::ByteOffset{at}));
    rig.workbench->language()->gotoDeclaration();
    ASSERT_TRUE(waitFor(
        [&] {
            auto* active = rig.window->tabs().currentEditor();
            return active && active->path() == dir.join("project/imported.props");
        },
        30s));
    EXPECT_TRUE(launcher.launches().empty()) << "Read-only .props/.targets navigation never evaluates MSBuild";
}
void cmakeRenameJourney(bool stale) {
    WorkDir dir(stale ? "cmk9-stale-preview" : "cmk9-atomic-preview");
    auto includePath = dir.join("project/extra.cmake");
    std::replace(includePath.begin(), includePath.end(), '\\', '/');
    const std::string source = "cmake_minimum_required(VERSION 3.25)\nproject(TargetRename LANGUAGES NONE)\n"
                               "add_custom_target(alpha)\nadd_custom_target(consumer)\ninclude(\"" +
                               includePath + "\")\n";
    const std::string extra = "add_dependencies(consumer alpha)\n";
    dir.write("project/CMakeLists.txt", source);
    dir.write("project/extra.cmake", extra);
    dir.write(
        "project/CMakePresets.json",
        R"({"version":3,"configurePresets":[{"name":"default","generator":"Ninja","binaryDir":"${sourceDir}/build"}]})");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    // Opening a trusted CMake folder starts configure. Wait for its result before renaming.
    ASSERT_TRUE(waitFor(
        [&] { return service->state() == frost::build::BuildState::Idle && service->lastConfigure().has_value(); },
        90s));
    const auto configured = *service->lastConfigure();
    ASSERT_TRUE(configured.succeeded()) << (configured.error ? configured.error->describe() : "CMake configure failed");
    ASSERT_TRUE(waitFor(
        [&] {
            return service->state() == frost::build::BuildState::Idle && service->model() && !service->modelStale() &&
                   rig.workbench->cmakeTargetsCurrent();
        },
        90s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project/CMakeLists.txt")));
    auto* view = rig.workbench->editors().find(dir.join("project/CMakeLists.txt"));
    ASSERT_NE(view, nullptr);
    view->editor().editor().setState(frost::text::EditorState::caretAt(frost::ByteOffset{source.find("alpha") + 1}));
    auto& popup = rig.workbench->language()->renameInput();
    rig.workbench->language()->rename();
    ASSERT_TRUE(waitFor([&] { return popup.isVisible(); }, 30s));
    popup.previewUsagesCheckbox()->setChecked(false);
    popup.searchCommentsCheckbox()->setChecked(false);
    bool sawPreview = false;
    QTimer preview;
    QObject::connect(&preview, &QTimer::timeout, [&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QDialog*>(widget);
                dialog && dialog->isVisible() && dialog->windowTitle() == QStringLiteral("Rename CMake target")) {
                preview.stop();
                sawPreview = true;
                if (stale)
                    EXPECT_TRUE(frost::io::realFileSystem().writeFileAtomic(dir.join("project/extra.cmake"),
                                                                            extra + "# foreign edit\n"));
                dialog->accept();
                return;
            }
    });
    preview.start(10);
    QString status;
    QObject::connect(rig.workbench->language(), &frost::ui::lang::LanguageController::statusMessage, rig.window.get(),
                     [&](const QString& message) { status = message; });
    popup.field()->setText(QStringLiteral("bravo"));
    frost::app::test::sendKey(popup.field(), Qt::Key_Return, Qt::NoModifier);
    ASSERT_TRUE(
        waitFor([&] { return sawPreview && (stale ? !status.isEmpty() : rig.workbench->canUndoRefactoring()); }, 60s));
    auto disk = frost::io::realFileSystem().readFile(dir.join("project/extra.cmake"));
    ASSERT_TRUE(disk);
    if (stale) {
        EXPECT_EQ(view->editor().editor().text().text(), source);
        EXPECT_EQ(*disk, extra + "# foreign edit\n");
        EXPECT_FALSE(rig.workbench->canUndoRefactoring());
    } else {
        EXPECT_NE(view->editor().editor().text().text().find("add_custom_target(bravo)"), std::string::npos);
        EXPECT_EQ(*disk, "add_dependencies(consumer bravo)\n");
        ASSERT_TRUE(rig.workbench->undoRefactoring());
        EXPECT_EQ(view->editor().editor().text().text(), source);
        EXPECT_EQ(frost::io::realFileSystem().readFile(dir.join("project/extra.cmake")).value(), extra);
    }
}
TEST(WorkbenchProjectLanguage, RealConfiguredTargetRenameIsPreviewedAndAtomicallyUndoable) {
    cmakeRenameJourney(false);
}
TEST(WorkbenchProjectLanguage, ChangedCMakeDependencyDuringPreviewRefusesAllParticipants) { cmakeRenameJourney(true); }
} // namespace
