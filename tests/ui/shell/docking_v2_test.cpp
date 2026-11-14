// SPDX-License-Identifier: Apache-2.0
// Own floating/docked widget ownership and layout restoration, without launching a feature process.
#include "frost/ui/shell/tool_window_manager.hpp"
#include <QLabel>
#include <gtest/gtest.h>
namespace frost::ui {
TEST(DockingV2, FloatingRetainsContentAndCoexistsWithDockedSide) {
    InMemoryShellSettings settings;
    QWidget host;
    auto* center = new QWidget;
    ToolWindowManager manager(host, *center, settings);
    ASSERT_TRUE(manager.add({.id = "first", .title = QStringLiteral("First"), .createContent = [] { return new QLabel(QStringLiteral("actual content")); }}));
    ASSERT_TRUE(manager.add({.id = "second", .title = QStringLiteral("Second")}));
    manager.show("first", false);
    auto* content = manager.content("first");
    manager.setFloating("first", true);
    ASSERT_TRUE(manager.isFloating("first"));
    ASSERT_NE(manager.floatingWindow("first"), nullptr);
    EXPECT_TRUE(manager.floatingWindow("first")->isAncestorOf(content));
    manager.show("second", false);
    EXPECT_TRUE(manager.isVisible("first"));
    EXPECT_TRUE(manager.isVisible("second"));
    manager.setFloating("first", false);
    EXPECT_EQ(manager.content("first"), content);
    EXPECT_FALSE(manager.isVisible("second"));
}
TEST(DockingV2, SideModeVisibilityAndGeometryRestoreFromSameSettings) {
    InMemoryShellSettings settings;
    {
        QWidget host;
        auto* center = new QWidget;
        ToolWindowManager manager(host, *center, settings);
        ASSERT_TRUE(manager.add({.id = "services", .title = QStringLiteral("Services")}));
        manager.moveToSide("services", ToolWindowSide::Right);
        manager.show("services", false);
        manager.setFloating("services", true);
        manager.hide("services");
        const auto* entry = settings.current().find("services");
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->side, 1);
        EXPECT_TRUE(entry->floating);
        EXPECT_FALSE(entry->geometry.empty());
    }
    QWidget host;
    auto* center = new QWidget;
    ToolWindowManager restored(host, *center, settings);
    ASSERT_TRUE(restored.add({.id = "services", .title = QStringLiteral("Services"), .side = ToolWindowSide::Left}));
    EXPECT_TRUE(restored.isFloating("services"));
    EXPECT_FALSE(restored.isVisible("services"));
    restored.show("services", false);
    EXPECT_TRUE(restored.isVisible("services"));
    EXPECT_EQ(settings.current().find("services")->side, 1);
}
} // namespace frost::ui
