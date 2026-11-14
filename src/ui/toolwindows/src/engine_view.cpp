// SPDX-License-Identifier: Apache-2.0
//
// Engine view rows and navigation over FEC-3 project.info/assets.list results.
#include "frost/ui/toolwindows/engine_view.hpp"

#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>

namespace frost::ui {
namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kHandleRole = Qt::UserRole + 1;

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

void assetRow(QTreeWidgetItem& parent, const engine::ProjectAsset& asset, engine::EngineHandle startScene) {
    auto* row = new QTreeWidgetItem(&parent);
    row->setText(0, qs(asset.path));
    row->setText(1, qs(asset.type));
    const QString handle = QString::number(static_cast<qulonglong>(asset.handle.value));
    row->setText(2, handle);
    row->setData(0, kPathRole, qs(asset.path));
    row->setData(0, kHandleRole, handle);
    if (asset.type == "Scene" && asset.handle == startScene)
        row->setToolTip(0, EngineView::tr("Project start scene"));
}

} // namespace

EngineView::EngineView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("engineView"));
    auto* layout = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("engineViewStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_refresh = new QPushButton(tr("Refresh Engine View"), this);
    m_refresh->setObjectName(QStringLiteral("engineViewRefresh"));
    layout->addWidget(m_refresh);
    connect(m_refresh, &QPushButton::clicked, this, &EngineView::refreshRequested);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("engineViewTree"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Project content"), tr("Type"), tr("Handle")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->setSortingEnabled(false); // Preserve the engine's order within each category.
    layout->addWidget(m_tree);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { (void)activate(item); });
    clear();
}

void EngineView::setContent(engine::ProjectContent content) {
    m_tree->clear();
    m_content = std::move(content);
    const auto& value = *m_content;
    m_status->setText(tr("%1 — %2").arg(qs(value.name), qs(value.projectFile)));
    auto* scenes = new QTreeWidgetItem(m_tree, {tr("Scenes (%1)").arg(static_cast<qulonglong>(value.sceneCount()))});
    auto* assets = new QTreeWidgetItem(m_tree, {tr("Assets (%1)").arg(static_cast<qulonglong>(value.assets.size()))});
    for (const auto& asset : value.assets) {
        if (asset.type == "Scene")
            assetRow(*scenes, asset, value.startScene);
        assetRow(*assets, asset, value.startScene);
    }
    auto* modules = new QTreeWidgetItem(m_tree, {tr("Project modules")});
    new QTreeWidgetItem(modules, {tr("Not declared by .frostproj format %1").arg(value.document.formatVersion)});
    scenes->setExpanded(true);
    assets->setExpanded(true);
    m_tree->setEnabled(true);
}

void EngineView::setUnavailable(const QString& reason) {
    m_content.reset();
    m_tree->clear();
    m_tree->setEnabled(false);
    m_status->setText(reason);
}

void EngineView::clear() { setUnavailable(tr("Engine project content is unavailable")); }

bool EngineView::activate(QTreeWidgetItem* item) {
    if (!m_content || !item || item->treeWidget() != m_tree)
        return false;
    const QString path = item->data(0, kPathRole).toString();
    const QString handle = item->data(0, kHandleRole).toString();
    const auto listed = std::ranges::find_if(m_content->assets, [&](const engine::ProjectAsset& asset) {
        return qs(asset.path) == path && QString::number(static_cast<qulonglong>(asset.handle.value)) == handle;
    });
    if (listed == m_content->assets.end())
        return false;
    emit openAsset(path, handle);
    return true;
}

} // namespace frost::ui
