// SPDX-License-Identifier: Apache-2.0
//
// The Configuration Manager (SLN-7): the active solution configuration and, per project, what it
// builds as in a solution configuration (project configuration, platform, Build), editable. The
// dialog collects edits; the caller writes them (frost::project::writeSolutionConfigurationEdits)
// through the apply callback, which runs on OK.
/*
 * Layout: "Active solution configuration" (cfgManagerActiveCombo) picks the configuration whose
 * mappings the table (cfgManagerProjectTable) shows: one row per loaded project, with the
 * project's configurations and platforms (its .vcxproj ProjectConfiguration items) in combo boxes
 * (cfgManagerConfiguration, cfgManagerPlatform; the mapped value is always listed, even one the
 * project does not declare) and a Build check box. In a .sln a project can also be left out of
 * the configuration ("(not mapped)", as an unmapped project is shown); a .slnx has no such form, so it is not
 * offered there. Projects FrostIDE does not load are listed without editors.
 *
 * Edits are kept per (solution configuration, project) while other configurations are viewed;
 * edits() returns those that differ from the solution as loaded. OK calls the apply callback with
 * them (skipped when there are none) and closes on success; on failure the error is shown in
 * cfgManagerError and the dialog stays open with its edits. Close discards everything.
 *
 * GUI thread only. The model must outlive the dialog.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/solution_configuration_edit.hpp"
#include "frost/project/solution_model.hpp"

#include <QDialog>

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QTableWidget;

namespace frost::ui {

class ConfigurationManagerDialog final : public QDialog {
    Q_OBJECT

public:
    using Apply = std::function<Result<void>(std::span<const project::ConfigurationMappingEdit>)>;

    ConfigurationManagerDialog(const project::SolutionModel& model, int activeConfiguration, Apply apply,
                               QWidget* parent = nullptr);
    ~ConfigurationManagerDialog() override;

    // The solution configuration chosen as active (the one shown).
    [[nodiscard]] int activeConfiguration() const;
    // The edits that differ from the loaded solution, in (configuration, project) order.
    [[nodiscard]] std::vector<project::ConfigurationMappingEdit> edits() const;
    // What the table shows for `project` in the shown configuration (edits included).
    [[nodiscard]] std::optional<project::ProjectConfigurationMapping> shownMapping(project::ProjectId project) const;
    // The table row of `project`, or -1 when it is not listed.
    [[nodiscard]] int rowOf(project::ProjectId project) const;

    // Shows configuration `index` (as choosing it in the combo does).
    void showConfiguration(int index);
    // Sets `project`'s mapping in the shown configuration through the row's editors; false when
    // the project has no editors or the value is not offered.
    bool setMapping(project::ProjectId project, const std::optional<project::ProjectConfigurationMapping>& mapping);

private:
    void populate();
    void onRowEdited(int row);
    void accept() override;

    const project::SolutionModel& m_model;
    Apply m_apply;
    QComboBox* m_active = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_error = nullptr;
    std::vector<project::ProjectId> m_rows; // Table row -> project.
    bool m_populating = false;
    // (configuration, project) -> the edited mapping.
    std::map<std::pair<std::size_t, std::uint32_t>, std::optional<project::ProjectConfigurationMapping>> m_edits;
};

} // namespace frost::ui
