// SPDX-License-Identifier: Apache-2.0
// RUN-11 R7: actual configuration dialog edit/copy/store preserves explicit mixed debugger choice.
#include "frost/io/path.hpp"
#include "frost/ui/run_configurations_dialog.hpp"

#include <QComboBox>
#include <QLineEdit>
#include <gtest/gtest.h>
namespace frost::ui {
TEST(EngineDebugModeDialog, MixedDebuggerChoiceSurvivesEditCopyAndStore) {
    run::RunConfigurationSet set;
    RunConfigurationsDialog dialog(set, "");
    ASSERT_FALSE(dialog.addFrostEditor().empty());
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/mixed" : "/mixed";
    auto* program = dialog.findChild<QLineEdit*>("runConfigurationProgram");
    auto* project = dialog.findChild<QLineEdit*>("runConfigurationEngineProject");
    auto* mode = dialog.findChild<QComboBox*>("runConfigurationEngineDebugMode");
    ASSERT_NE(program, nullptr);
    ASSERT_NE(project, nullptr);
    ASSERT_NE(mode, nullptr);
    program->setText(QString::fromStdString(io::normalizePath(root + "/FrostEditor.exe").value_or("")));
    project->setText(QString::fromStdString(io::normalizePath(root + "/Game.frostproj").value_or("")));
    mode->setCurrentIndex(mode->findData(static_cast<int>(run::EngineDebugMode::NativeAndLua)));
    ASSERT_FALSE(dialog.copySelected().empty());
    dialog.accept();
    const auto edited = dialog.edited();
    ASSERT_EQ(edited.size(), 2u);
    for (const auto& c : edited)
        EXPECT_EQ(c.engineDebugMode, run::EngineDebugMode::NativeAndLua);
    auto parsed = run::parseRunConfigurations(run::serializeRunConfigurations(edited));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, edited);
}
} // namespace frost::ui
