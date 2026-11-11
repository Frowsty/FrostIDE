// SPDX-License-Identifier: Apache-2.0
//
// FEC-3 Engine view: exact content/counts, read-only navigation, and clearing unavailable results.
#include "frost/ui/toolwindows/engine_view.hpp"

#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <vector>

namespace frost::ui {
namespace {

engine::ProjectContent content() {
    engine::ProjectContent model;
    model.document.formatVersion = 1;
    model.name = "Game";
    model.projectFile = "Game.frostproj";
    model.startScene = engine::EngineHandle{16273909378015378282ULL};
    model.assets = {{model.startScene, "Assets/Scenes/Main.fscene", "Scene"},
                    {engine::EngineHandle{42}, "Assets/Scripts/Player.lua", "Script"}};
    return model;
}

TEST(EngineView, ListsSceneAssetsAndAllAssetsWithExactHandles) {
    EngineView view;
    const auto model = content();
    view.setContent(model);
    ASSERT_EQ(view.tree()->topLevelItemCount(), 3);
    auto* scenes = view.tree()->topLevelItem(0);
    auto* assets = view.tree()->topLevelItem(1);
    EXPECT_EQ(scenes->text(0), QStringLiteral("Scenes (1)"));
    EXPECT_EQ(assets->text(0), QStringLiteral("Assets (2)"));
    ASSERT_EQ(scenes->childCount(), static_cast<int>(model.sceneCount()));
    ASSERT_EQ(assets->childCount(), static_cast<int>(model.assets.size()));
    for (int i = 0; i < assets->childCount(); ++i) {
        const auto& source = model.assets[static_cast<std::size_t>(i)];
        EXPECT_EQ(assets->child(i)->text(0), QString::fromStdString(source.path));
        EXPECT_EQ(assets->child(i)->text(1), QString::fromStdString(source.type));
        EXPECT_EQ(assets->child(i)->text(2), QString::number(static_cast<qulonglong>(source.handle.value)));
    }
    EXPECT_EQ(scenes->child(0)->text(2), QStringLiteral("16273909378015378282"));
    EXPECT_EQ(scenes->child(0)->toolTip(0), QStringLiteral("Project start scene"));
    EXPECT_FALSE(assets->child(0)->flags().testFlag(Qt::ItemIsEditable));
    EXPECT_EQ(view.tree()->topLevelItem(2)->child(0)->text(0), QStringLiteral("Not declared by .frostproj format 1"));
}

TEST(EngineView, NavigationEmitsOnlyListedAssetIdentity) {
    EngineView view;
    view.setContent(content());
    std::vector<std::pair<QString, QString>> opened;
    QObject::connect(&view, &EngineView::openAsset,
                     [&](const QString& path, const QString& handle) { opened.emplace_back(path, handle); });
    EXPECT_FALSE(view.activate(view.tree()->topLevelItem(0)));
    ASSERT_TRUE(view.activate(view.tree()->topLevelItem(0)->child(0)));
    ASSERT_EQ(opened.size(), 1U);
    EXPECT_EQ(opened[0].first, QStringLiteral("Assets/Scenes/Main.fscene"));
    EXPECT_EQ(opened[0].second, QStringLiteral("16273909378015378282"));
    QTreeWidget foreign;
    auto* detached = new QTreeWidgetItem(&foreign, {QStringLiteral("Assets/Scenes/Main.fscene")});
    EXPECT_FALSE(view.activate(detached));
}

TEST(EngineView, UnavailableStateClearsCountsAndNavigationAndUsesPlainText) {
    EngineView view;
    view.setContent(content());
    view.setUnavailable(QStringLiteral("<b>FrostEditor tool unavailable</b>"));
    EXPECT_FALSE(view.content());
    EXPECT_EQ(view.tree()->topLevelItemCount(), 0);
    EXPECT_FALSE(view.tree()->isEnabled());
    EXPECT_EQ(view.statusLabel()->textFormat(), Qt::PlainText);
    EXPECT_EQ(view.statusLabel()->text(), QStringLiteral("<b>FrostEditor tool unavailable</b>"));
    EXPECT_FALSE(view.activate(nullptr));
    view.setContent(content());
    EXPECT_TRUE(view.tree()->isEnabled());
    view.clear();
    EXPECT_FALSE(view.content());
    EXPECT_EQ(view.tree()->topLevelItemCount(), 0);
}

TEST(EngineView, RefreshRequestsWorkWithoutInventingContent) {
    EngineView view;
    int refreshes = 0;
    QObject::connect(&view, &EngineView::refreshRequested, [&] { ++refreshes; });
    auto* refresh = view.findChild<QPushButton*>(QStringLiteral("engineViewRefresh"));
    ASSERT_NE(refresh, nullptr);
    refresh->click();
    EXPECT_EQ(refreshes, 1);
    EXPECT_FALSE(view.content());
    EXPECT_EQ(view.tree()->topLevelItemCount(), 0);
}

} // namespace
} // namespace frost::ui
