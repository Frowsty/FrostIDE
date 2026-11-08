// SPDX-License-Identifier: Apache-2.0
//
// RunConfigurationsDialog: Run > Edit Configurations... (RUN-3, PLAN.md Appendix C R2), backed by
// RUN-1's model. It replaces the R0 dialog, whose debugger settings were read-only placeholders.
//
/*
 * Layout, as Rider's: the configurations on the left (generated ones marked "generated"), with Add
 * (a native configuration), Copy and Remove; the selected one's fields on the right: name, type,
 * target, program, program arguments (one string with Windows command-line quoting, split into argv
 * with the MSVC CRT's rules, build::splitWindowsArguments), working directory, environment (one
 * NAME=VALUE per line; a line with just NAME removes the variable) and the before-launch step.
 *
 * What can be edited, by type (run_types.hpp):
 *   - native: everything; the program is an absolute path.
 *   - cmake-target: name, arguments, working directory and environment. The program comes from the
 *     target's artifact after the build, so it is shown as such and cannot be typed.
 *   - msbuild: the name only. MSBuild projects run with the debugger settings MSBuild evaluates for
 *     them (LocalDebuggerCommandArguments, ...WorkingDirectory, ...Environment of the .vcxproj and
 *     .vcxproj.user); writing those to the .vcxproj.user is SLN-11's, so the fields say so and
 *     stay read-only.
 * Generated configurations cannot be removed (the next generation brings them back); Remove is
 * disabled for them. Compound runs are shown disabled: they are RUN-5's.
 *
 * OK validates every configuration (run::validateRunConfiguration) and refuses with the first
 * error shown in the dialog; edited() is then the list to hand to RunController::
 * replaceConfigurations. Cancel leaves nothing changed. GUI thread only.
 */
#pragma once

#include "frost/run/run_configuration.hpp"
#include "frost/run/run_configuration_set.hpp"

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
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

    void accept() override;

private:
    void showConfiguration(int row);
    void storeFields();
    void refreshList();

    const run::RunConfigurationSet& m_set;
    std::vector<run::RunConfiguration> m_configurations;
    bool m_canAddNative = true;
    int m_row = -1;
    bool m_loading = false;
    QListWidget* m_list = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_remove = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_type = nullptr;
    QLabel* m_target = nullptr;
    QLineEdit* m_program = nullptr;
    QLineEdit* m_arguments = nullptr;
    QLineEdit* m_workingDirectory = nullptr;
    QPlainTextEdit* m_environment = nullptr;
    QLabel* m_beforeLaunch = nullptr;
    QLabel* m_note = nullptr;
    QLabel* m_error = nullptr;
};

} // namespace frost::ui
