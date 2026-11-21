// SPDX-License-Identifier: Apache-2.0
// Own floating/docked widget ownership and layout restoration, without launching a feature process.
#include "frost/ui/shell/tool_window_manager.hpp"

#include <QLabel>
#include <QToolButton>
#include <gtest/gtest.h>
namespace frost::ui {
namespace {
ToolWindowSpec toolWindow(std::string id, QString title, std::function<QWidget*()> createContent = {}) {
    ToolWindowSpec spec;
    spec.id = std::move(id);
    spec.title = std::move(title);
    spec.createContent = std::move(createContent);
    return spec;
}
} // namespace

TEST(DockingV2, FloatingRetainsContentAndCoexistsWithDockedSide) {
    InMemoryShellSettings settings;
    QWidget host;
    auto* center = new QWidget;
    ToolWindowManager manager(host, *center, settings);
    ASSERT_TRUE(manager.add(
        toolWindow("first", QStringLiteral("First"), [] { return new QLabel(QStringLiteral("actual content")); })));
    ASSERT_TRUE(manager.add(toolWindow("second", QStringLiteral("Second"))));
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
        ASSERT_TRUE(manager.add(toolWindow("services", QStringLiteral("Services"))));
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
    ASSERT_TRUE(restored.add(toolWindow("services", QStringLiteral("Services"))));
    EXPECT_TRUE(restored.isFloating("services"));
    EXPECT_FALSE(restored.isVisible("services"));
    restored.show("services", false);
    EXPECT_TRUE(restored.isVisible("services"));
    EXPECT_EQ(settings.current().find("services")->side, 1);
}
TEST(DockingV2, HiddenRestoreDoesNotOverwriteSavedModeSizeOrPin) {
    ShellLayout initial;
    auto& saved = initial.entry("services");
    saved.side = 1;
    saved.floating = true;
    saved.size = 417;
    saved.pinned = false;
    saved.visible = false;
    InMemoryShellSettings settings(initial);
    QWidget host;
    auto* center = new QWidget;
    ToolWindowManager manager(host, *center, settings);
    ASSERT_TRUE(manager.add(toolWindow("services", QStringLiteral("Services"))));
    EXPECT_EQ(settings.storeCount(), 0U);
    EXPECT_EQ(settings.current(), initial);
    EXPECT_TRUE(manager.isFloating("services"));
    EXPECT_EQ(manager.size("services"), 417);
    manager.show("services", false);
    const auto* restored = settings.current().find("services");
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->side, 1);
    EXPECT_EQ(restored->size, 417);
    EXPECT_EQ(restored->pinned, false);
}
TEST(DockingV2, AutomaticRevealKeepsSavedLayoutUntilAnExplicitToggle) {
    ShellLayout initial;
    auto& existing = initial.entry("existing");
    existing.side = 1;
    existing.visible = true;
    auto& automatic = initial.entry("automatic");
    automatic.side = 1;
    automatic.size = 417;
    automatic.visible = false;
    automatic.pinned = false;
    InMemoryShellSettings settings(initial);
    {
        QWidget host;
        auto* center = new QWidget;
        ToolWindowManager manager(host, *center, settings);
        ASSERT_TRUE(manager.add(toolWindow("existing", QStringLiteral("Existing"))));
        ASSERT_TRUE(manager.add(toolWindow("automatic", QStringLiteral("Automatic"))));

        manager.showTransient("automatic");
        EXPECT_TRUE(manager.isVisible("automatic"));
        EXPECT_FALSE(manager.isVisible("existing"));
        EXPECT_EQ(manager.size("automatic"), 417);
        EXPECT_EQ(settings.current(), initial);
        EXPECT_EQ(settings.storeCount(), 0U);
    }
    QWidget host;
    auto* center = new QWidget;
    ToolWindowManager restored(host, *center, settings);
    ASSERT_TRUE(restored.add(toolWindow("existing", QStringLiteral("Existing"))));
    ASSERT_TRUE(restored.add(toolWindow("automatic", QStringLiteral("Automatic"))));
    EXPECT_TRUE(restored.isVisible("existing"));
    EXPECT_FALSE(restored.isVisible("automatic"));
    EXPECT_EQ(restored.size("automatic"), 417);
    EXPECT_EQ(settings.storeCount(), 0U);
    restored.toggle("automatic");
    ASSERT_EQ(settings.storeCount(), 1U);
    const auto* saved = settings.current().find("automatic");
    ASSERT_NE(saved, nullptr);
    EXPECT_TRUE(saved->visible);
    EXPECT_EQ(saved->side, 1);
    EXPECT_EQ(saved->pinned, false);
}
TEST(DockingV2, ShowingUnpinnedFloatingWindowRestoresItsStripe) {
    ShellLayout initial;
    auto& saved = initial.entry("services");
    saved.side = 1;
    saved.floating = true;
    saved.pinned = false;
    saved.visible = false;
    InMemoryShellSettings settings(initial);
    QWidget host;
    auto* center = new QWidget;
    ToolWindowManager manager(host, *center, settings);
    ASSERT_TRUE(manager.add(toolWindow("services", QStringLiteral("Services"))));
    host.show();
    auto* button = manager.stripeButton("services");
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(button->isVisibleTo(&host));
    manager.show("services", false);
    EXPECT_TRUE(button->isVisibleTo(&host));
    manager.hide("services");
    EXPECT_FALSE(button->isVisibleTo(&host));
}
} // namespace frost::ui
