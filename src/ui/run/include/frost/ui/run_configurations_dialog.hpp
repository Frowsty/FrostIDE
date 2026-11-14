// SPDX-License-Identifier: Apache-2.0
//
// RunConfigurationsDialog: Run > Edit Configurations... (RUN-3, PLAN.md Appendix C R2), backed by
// RUN-1's model. It replaces the R0 dialog, whose debugger settings were read-only placeholders.
// RUN-5 (R3) adds before-launch tasks, compound configurations and attach configurations.
//
/*
 * Layout, as Rider's: the configurations on the left (generated ones marked "generated"), with Add
 * (a native configuration), Copy and Remove; the selected one's fields on the right: name, type,
 * target, program, program arguments (one string with Windows command-line quoting, split into argv
 * with the MSVC CRT's rules, build::splitWindowsArguments), working directory, environment (one
 * NAME=VALUE per line; a line with just NAME removes the variable), the before-launch step, and the
 * before-launch tasks (RUN-5): one command line per line, the absolute program first, with the same
 * quoting as the arguments; a task's own working directory (from the store) is kept while its line
 * is unchanged.
 *
 * What can be edited, by type (run_types.hpp):
 *   - native: everything; the program is an absolute path.
 *   - frost-editor / frost-player (RUN-11): executable (selecting the built engine configuration),
 *     project directory or .frostproj, arguments, working directory, environment and tasks.
 *     The engine project field supplies --project; it cannot be overridden by the arguments.
 *   - cmake-target: name, arguments, working directory and environment. The program comes from the
 *     target's artifact after the build, so it is shown as such and cannot be typed.
 *   - msbuild: the name only. MSBuild projects run with the debugger settings MSBuild evaluates for
 *     them (LocalDebuggerCommandArguments, ...WorkingDirectory, ...Environment of the .vcxproj and
 *     .vcxproj.user); writing those to the .vcxproj.user is SLN-11's, so the fields say so and
 *     stay read-only. They take no before-launch tasks (the RunService runs them).
 *   - compound (RUN-5, "Add Compound..."): the name and the members: a checkable list of the other
 *     native, CMake and MSBuild configurations; checked ones start, in the list's order.
 *   - attach (RUN-5, "Add Attach..."): the name and the process: its executable's file name or
 *     absolute path. Debug attaches to the one running process that matches.
 * Generated configurations cannot be removed (the next generation brings them back); Remove is
 * disabled for them. Copy keeps compound, attach and FrostEngine types; any other becomes a
 * native copy.
 *
 * OK validates every configuration (run::validateRunConfiguration) and refuses with the first
 * error shown in the dialog; edited() is then the list to hand to RunController::
 * replaceConfigurations. Cancel leaves nothing changed. GUI thread only.
 */
#pragma once

#include "frost/run/run_configuration.hpp"
#include "frost/run/run_configuration_set.hpp"
#include "frost/run/vs_import.hpp"
#include <functional>

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QComboBox;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace frost::ui {

// The argument list as one string the dialog shows: each argument quoted by Windows command-line
// rules where needed (io::appendWindowsArgument), joined by spaces. Pure.
[[nodiscard]] QString formatRunArguments(const std::vector<std::string>& arguments);
// The inverse: the MSVC CRT's split (build::splitWindowsArguments). Pure.
[[nodiscard]] std::vector<std::string> parseRunArguments(const QString& text);
// The environment as NAME=VALUE lines (NAME alone for a removal). Pure.
[[nodiscard]] QString formatRunEnvironment(const std::vector<run::EnvironmentEntry>& environment);
// The inverse: blank lines are skipped, NAME=VALUE sets (the value may hold '='), NAME removes. Pure.
[[nodiscard]] std::vector<run::EnvironmentEntry> parseRunEnvironment(const QString& text);

class RunConfigurationsDialog final : public QDialog {
    Q_OBJECT

public:
    // Shows `set`'s merged configurations, `selectedId` selected (else the first). `canAddNative`
    // false (no workspace to store them in) disables Add and Copy.
    RunConfigurationsDialog(const run::RunConfigurationSet& set, const std::string& selectedId,
                            bool canAddNative = true, QWidget* parent = nullptr);
    ~RunConfigurationsDialog() override;
    RunConfigurationsDialog(const RunConfigurationsDialog&) = delete ("a QWidget");
    RunConfigurationsDialog& operator=(const RunConfigurationsDialog&) = delete ("a QWidget");

    // The configurations as edited so far (the fields of the selected one included).
    [[nodiscard]] std::vector<run::RunConfiguration> edited();
    // The id of the configuration selected when the dialog closed ("" when there is none).
    [[nodiscard]] std::string selectedId() const;

    // Adds a native configuration (Add), selects it and returns its id.
    std::string addNative();
    // Copies the selected configuration as a native one (Copy; a generated one keeps its fields
    // and the program the user types). Returns the new id, or "" when nothing is selected.
    std::string copySelected();
    // Removes the selected configuration if it is not generated; returns whether it did.
    bool removeSelected();
    // Adds an empty compound configuration ("Add Compound...") or an attach configuration ("Add
    // Attach..."), selects it and returns its id ("" when nothing can be added).
    std::string addCompound();
    std::string addAttach();
    // RUN-11: add a typed editor/player configuration, with editable executable and project.
    std::string addFrostEditor();
    std::string addFrostPlayer();

    using ImportHandler = std::function<Result<run::RunImportReport>(const std::string& path,
        std::span<const run::RunConfiguration> current)>;
    void setImportHandler(ImportHandler handler);
    [[nodiscard]] Result<run::RunImportReport> importFile(const std::string& path);
    void accept() override;

private:
    void showConfiguration(int row);
    void storeFields();
    void refreshList();
    std::string addOfType(run::RunType type);
    void fillMembers(const run::RunConfiguration& configuration);

    const run::RunConfigurationSet& m_set;
    std::vector<run::RunConfiguration> m_configurations;
    bool m_canAddNative = true;
    int m_row = -1;
    bool m_loading = false;
    ImportHandler m_importHandler;
    QPushButton* m_import = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_remove = nullptr;
    QListWidget* m_list = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_type = nullptr;
    QLabel* m_target = nullptr;
    QLineEdit* m_program = nullptr;
    QLineEdit* m_engineProject = nullptr;
    QComboBox* m_engineDebugMode = nullptr;
    QPushButton* m_addFrostEditor = nullptr;
    QPushButton* m_addFrostPlayer = nullptr;
    QLineEdit* m_arguments = nullptr;
    QLineEdit* m_workingDirectory = nullptr;
    QPlainTextEdit* m_environment = nullptr;
    QLabel* m_beforeLaunch = nullptr;
    QPlainTextEdit* m_tasks = nullptr;
    QListWidget* m_members = nullptr;
    QLineEdit* m_process = nullptr;
    QFormLayout* m_form = nullptr;
    QPushButton* m_addCompound = nullptr;
    QPushButton* m_addAttach = nullptr;
    QLabel* m_note = nullptr;
    QLabel* m_error = nullptr;
};

} // namespace frost::ui
