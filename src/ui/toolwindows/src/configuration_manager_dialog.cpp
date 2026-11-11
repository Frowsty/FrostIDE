// SPDX-License-Identifier: Apache-2.0
//
// ConfigurationManagerDialog (frost/ui/toolwindows/configuration_manager_dialog.hpp, SLN-7): the
// editable project configuration mappings of a solution.

#include "frost/ui/toolwindows/configuration_manager_dialog.hpp"

#include "frost/ui/dialog_chrome.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace frost::ui {
namespace {

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

constexpr int kProjectColumn = 0;
constexpr int kConfigurationColumn = 1;
constexpr int kPlatformColumn = 2;
constexpr int kBuildColumn = 3;

// What the table shows for a project without a mapping, and the .sln combo entry that removes one.
QString notInConfiguration() { return QStringLiteral("(not mapped)"); }

void addUnique(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) {
        return;
    }
    const bool present = std::ranges::any_of(
        values, [&](const std::string& v) { return QString::compare(qs(v), qs(value), Qt::CaseInsensitive) == 0; });
    if (!present) {
        values.push_back(value);
    }
}

int indexOf(const QComboBox* combo, const std::string& value) {
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i).toString().compare(qs(value), Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

} // namespace

ConfigurationManagerDialog::ConfigurationManagerDialog(const project::SolutionModel& model, int activeConfiguration,
                                                       Apply apply, QWidget* parent)
    : QDialog(parent), m_model(model), m_apply(std::move(apply)), m_active(new QComboBox(this)) {
    setObjectName(QStringLiteral("configurationManagerDialog"));
    setWindowTitle(QStringLiteral("Configuration Manager"));
    setModal(true);
    resize(680, 420);
    auto* outer = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("Active solution configuration:"), this));
    m_active->setObjectName(QStringLiteral("cfgManagerActiveCombo"));
    for (const project::SolutionConfiguration& configuration : model.configurations()) {
        m_active->addItem(qs(configuration.name()));
    }
    if (activeConfiguration >= 0 && activeConfiguration < m_active->count()) {
        m_active->setCurrentIndex(activeConfiguration);
    }
    top->addWidget(m_active, 1);
    outer->addLayout(top);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("cfgManagerProjectTable"));
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Project"), QStringLiteral("Configuration"),
                                        QStringLiteral("Platform"), QStringLiteral("Build")});
    m_table->horizontalHeader()->setSectionResizeMode(kProjectColumn, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    outer->addWidget(m_table, 1);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("cfgManagerError"));
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    outer->addWidget(m_error);

    // OK writes the edits and applies the active configuration; Close discards both.
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConfigurationManagerDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    for (std::size_t p = 0; p < model.projects().size(); ++p) {
        m_rows.emplace_back(static_cast<std::uint32_t>(p));
    }
    connect(m_active, &QComboBox::currentIndexChanged, this, [this](int) { populate(); });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!m_populating && item != nullptr && item->column() == kBuildColumn) {
            onRowEdited(item->row());
        }
    });
    populate();
    installDialogChrome(this, true);
}

ConfigurationManagerDialog::~ConfigurationManagerDialog() = default;

int ConfigurationManagerDialog::activeConfiguration() const { return m_active->currentIndex(); }

int ConfigurationManagerDialog::rowOf(project::ProjectId project) const {
    const auto found = std::ranges::find(m_rows, project);
    return found == m_rows.end() ? -1 : static_cast<int>(found - m_rows.begin());
}

void ConfigurationManagerDialog::showConfiguration(int index) { m_active->setCurrentIndex(index); }

std::vector<project::ConfigurationMappingEdit> ConfigurationManagerDialog::edits() const {
    std::vector<project::ConfigurationMappingEdit> out;
    for (const auto& [key, mapping] : m_edits) {
        out.push_back(
            {.project = project::ProjectId{key.second}, .solutionConfiguration = key.first, .mapping = mapping});
    }
    return out;
}

std::optional<project::ProjectConfigurationMapping>
ConfigurationManagerDialog::shownMapping(project::ProjectId project) const {
    const int configuration = m_active->currentIndex();
    if (configuration < 0 || project.value() >= m_model.projects().size()) {
        return std::nullopt;
    }
    if (const auto edited = m_edits.find({static_cast<std::size_t>(configuration), project.value()});
        edited != m_edits.end()) {
        return edited->second;
    }
    return m_model.projectConfiguration(project, static_cast<std::size_t>(configuration));
}

void ConfigurationManagerDialog::populate() {
    m_populating = true;
    const int configuration = m_active->currentIndex();
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(m_rows.size()));
    const bool sln = m_model.format() == project::SolutionFormat::Sln;
    for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
        const project::ProjectId id = m_rows[static_cast<std::size_t>(row)];
        const project::ProjectInfo& info = m_model.project(id);
        auto* name = new QTableWidgetItem(qs(info.name));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, kProjectColumn, name);
        const std::optional<project::ProjectConfigurationMapping> mapping =
            configuration >= 0 ? shownMapping(id) : std::nullopt;
        auto* configurationText = new QTableWidgetItem(mapping ? qs(mapping->configuration) : notInConfiguration());
        auto* platformText = new QTableWidgetItem(mapping ? qs(mapping->platform) : notInConfiguration());
        configurationText->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        platformText->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, kConfigurationColumn, configurationText);
        m_table->setItem(row, kPlatformColumn, platformText);
        auto* build = new QTableWidgetItem(mapping ? QString() : notInConfiguration());
        const bool editable = info.nodeKind == project::NodeKind::Project && configuration >= 0;
        build->setFlags(editable ? (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) : Qt::ItemIsUserCheckable);
        build->setCheckState(mapping && mapping->build ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(row, kBuildColumn, build);
        if (!editable) {
            continue;
        }
        // The values offered: the project's own ProjectConfiguration items, and the mapped value.
        std::vector<std::string> configurations;
        std::vector<std::string> platforms;
        if (info.vcxproj) {
            for (const project::ProjectConfiguration& declared : info.vcxproj->configurations) {
                addUnique(configurations, declared.configuration);
                addUnique(platforms, declared.platform);
            }
        }
        if (const auto& loaded = m_model.projectConfiguration(id, static_cast<std::size_t>(configuration))) {
            addUnique(configurations, loaded->configuration);
            addUnique(platforms, loaded->platform);
        }
        if (mapping) {
            addUnique(configurations, mapping->configuration);
            addUnique(platforms, mapping->platform);
        }
        auto* configurationCombo = new QComboBox(m_table);
        configurationCombo->setObjectName(QStringLiteral("cfgManagerConfiguration"));
        if (sln) {
            configurationCombo->addItem(notInConfiguration(), QString());
        }
        for (const std::string& value : configurations) {
            configurationCombo->addItem(qs(value), qs(value));
        }
        auto* platformCombo = new QComboBox(m_table);
        platformCombo->setObjectName(QStringLiteral("cfgManagerPlatform"));
        for (const std::string& value : platforms) {
            platformCombo->addItem(qs(value), qs(value));
        }
        configurationCombo->setCurrentIndex(mapping ? indexOf(configurationCombo, mapping->configuration) : 0);
        platformCombo->setCurrentIndex(mapping ? indexOf(platformCombo, mapping->platform) : 0);
        platformCombo->setEnabled(mapping.has_value());
        m_table->setCellWidget(row, kConfigurationColumn, configurationCombo);
        m_table->setCellWidget(row, kPlatformColumn, platformCombo);
        connect(configurationCombo, &QComboBox::currentIndexChanged, this, [this, row](int) {
            if (!m_populating) {
                onRowEdited(row);
            }
        });
        connect(platformCombo, &QComboBox::currentIndexChanged, this, [this, row](int) {
            if (!m_populating) {
                onRowEdited(row);
            }
        });
    }
    m_populating = false;
}

void ConfigurationManagerDialog::onRowEdited(int row) {
    const int configuration = m_active->currentIndex();
    if (configuration < 0 || row < 0 || row >= static_cast<int>(m_rows.size())) {
        return;
    }
    const project::ProjectId id = m_rows[static_cast<std::size_t>(row)];
    auto* configurationCombo = qobject_cast<QComboBox*>(m_table->cellWidget(row, kConfigurationColumn));
    auto* platformCombo = qobject_cast<QComboBox*>(m_table->cellWidget(row, kPlatformColumn));
    QTableWidgetItem* build = m_table->item(row, kBuildColumn);
    if (configurationCombo == nullptr || platformCombo == nullptr || build == nullptr) {
        return;
    }
    const auto& loaded = m_model.projectConfiguration(id, static_cast<std::size_t>(configuration));
    std::optional<project::ProjectConfigurationMapping> mapping;
    const QString chosen = configurationCombo->currentData().toString();
    if (!chosen.isEmpty()) {
        mapping =
            project::ProjectConfigurationMapping{.configuration = chosen.toStdString(),
                                                 .platform = platformCombo->currentData().toString().toStdString(),
                                                 .build = build->checkState() == Qt::Checked,
                                                 .deploy = loaded ? loaded->deploy : false};
        if (mapping->platform.empty() && loaded) {
            mapping->platform = loaded->platform;
        }
    }
    platformCombo->setEnabled(mapping.has_value());
    m_populating = true;
    if (QTableWidgetItem* text = m_table->item(row, kConfigurationColumn)) {
        text->setText(mapping ? qs(mapping->configuration) : notInConfiguration());
    }
    if (QTableWidgetItem* text = m_table->item(row, kPlatformColumn)) {
        text->setText(mapping ? qs(mapping->platform) : notInConfiguration());
    }
    build->setText(mapping ? QString() : notInConfiguration());
    m_populating = false;
    const std::pair key{static_cast<std::size_t>(configuration), id.value()};
    if (mapping == loaded) {
        m_edits.erase(key);
    } else {
        m_edits[key] = mapping;
    }
    m_error->setVisible(false);
}

bool ConfigurationManagerDialog::setMapping(project::ProjectId project,
                                            const std::optional<project::ProjectConfigurationMapping>& mapping) {
    const int row = rowOf(project);
    if (row < 0) {
        return false;
    }
    auto* configurationCombo = qobject_cast<QComboBox*>(m_table->cellWidget(row, kConfigurationColumn));
    auto* platformCombo = qobject_cast<QComboBox*>(m_table->cellWidget(row, kPlatformColumn));
    QTableWidgetItem* build = m_table->item(row, kBuildColumn);
    if (configurationCombo == nullptr || platformCombo == nullptr || build == nullptr) {
        return false;
    }
    const int configurationIndex =
        mapping ? indexOf(configurationCombo, mapping->configuration) : configurationCombo->findData(QString());
    const int platformIndex = mapping ? indexOf(platformCombo, mapping->platform) : platformCombo->currentIndex();
    if (configurationIndex < 0 || platformIndex < 0) {
        return false;
    }
    m_populating = true;
    configurationCombo->setCurrentIndex(configurationIndex);
    platformCombo->setCurrentIndex(platformIndex);
    if (mapping) {
        build->setCheckState(mapping->build ? Qt::Checked : Qt::Unchecked);
    }
    m_populating = false;
    onRowEdited(row);
    return true;
}

void ConfigurationManagerDialog::accept() {
    const std::vector<project::ConfigurationMappingEdit> pending = edits();
    if (!pending.empty() && m_apply) {
        if (auto applied = m_apply(pending); !applied) {
            m_error->setText(QStringLiteral("The solution was not changed: %1").arg(qs(applied.error().message())));
            m_error->setVisible(true);
            return;
        }
    }
    QDialog::accept();
}

} // namespace frost::ui
