// SPDX-License-Identifier: Apache-2.0
//
// The helpers the language-feature widgets share (src/ui/lang/src/lang_widgets_internal.hpp,
// FND-12a), offscreen: the item data roles are distinct user roles, fileName takes the last
// component of either separator, the toggle buttons are checkable auto-raise buttons, and
// placeNear puts a popup below its anchor, above it when there is no room below, inside the
// anchor's screen. The expected positions come from the offscreen screen's own geometry.

#include "../../../src/ui/lang/src/lang_widgets_internal.hpp"
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>

namespace {

namespace lang = frost::ui::lang;

TEST(LangWidgetsInternal, ItemRolesAreDistinctUserRoles) {
    const std::array<int, 6> roles{lang::kDetailRole,     lang::kItemRole,     lang::kTargetRole,
                                   lang::kRangeStartRole, lang::kRangeEndRole, lang::kPathRole};
    for (const int role : roles)
        EXPECT_GT(role, Qt::UserRole);
    std::array<int, 6> sorted = roles;
    std::ranges::sort(sorted);
    EXPECT_EQ(std::ranges::adjacent_find(sorted), sorted.end());
}

TEST(LangWidgetsInternal, FileNameTakesTheLastComponent) {
    EXPECT_EQ(lang::fileName("C:/src/engine/main.cpp"), "main.cpp");
    EXPECT_EQ(lang::fileName("C:\\src\\engine\\render.h"), "render.h");
    EXPECT_EQ(lang::fileName("/home/u/mixed\\dir/file.lua"), "file.lua");
    EXPECT_EQ(lang::fileName("plain.txt"), "plain.txt");
    EXPECT_EQ(lang::fileName("dir/"), "");
    EXPECT_EQ(lang::fileName(""), "");
}

TEST(LangWidgetsInternal, ToggleToolButtonIsCheckableAutoRaiseAndOwnedByItsParent) {
    QWidget parent;
    QToolButton* on = lang::makeToggleToolButton(&parent, QStringLiteral("Aa"), QStringLiteral("Match case"), true);
    QToolButton* off = lang::makeToggleToolButton(&parent, QStringLiteral("W"), QStringLiteral("Words"), false);
    ASSERT_NE(on, nullptr);
    EXPECT_EQ(on->parent(), &parent);
    EXPECT_EQ(on->text(), QStringLiteral("Aa"));
    EXPECT_EQ(on->toolTip(), QStringLiteral("Match case"));
    EXPECT_TRUE(on->isCheckable());
    EXPECT_TRUE(on->isChecked());
    EXPECT_TRUE(on->autoRaise());
    EXPECT_TRUE(off->isCheckable());
    EXPECT_FALSE(off->isChecked());
    off->click();
    EXPECT_TRUE(off->isChecked());
}

class PlaceNearTest : public ::testing::Test {
protected:
    void SetUp() override {
        QScreen* screen = QGuiApplication::primaryScreen();
        ASSERT_NE(screen, nullptr) << "the offscreen QPA platform has one screen";
        m_area = screen->availableGeometry();
        ASSERT_GE(m_area.width(), 400) << "the placement cases need a screen of at least 400x300";
        ASSERT_GE(m_area.height(), 300) << "the placement cases need a screen of at least 400x300";
        m_popup = std::make_unique<QWidget>();
        m_popup->resize(120, 60);
    }

    QRect m_area;
    std::unique_ptr<QWidget> m_popup;
};

TEST_F(PlaceNearTest, BelowTheAnchorWhenThereIsRoom) {
    const QRect anchor(m_area.left() + 50, m_area.top() + 40, 30, 16);
    lang::placeNear(m_popup.get(), anchor, false);
    EXPECT_EQ(m_popup->pos(), QPoint(anchor.left(), anchor.bottom() + 2));
}

TEST_F(PlaceNearTest, AboveTheAnchorWhenAskedAndThereIsRoom) {
    const QRect anchor(m_area.left() + 50, m_area.top() + 200, 30, 16);
    lang::placeNear(m_popup.get(), anchor, true);
    EXPECT_EQ(m_popup->pos(), QPoint(anchor.left(), anchor.top() - m_popup->height() - 1));
}

TEST_F(PlaceNearTest, FlipsAboveWhenTheScreenEndsBelowTheAnchor) {
    const QRect anchor(m_area.left() + 50, m_area.bottom() - 30, 30, 16);
    lang::placeNear(m_popup.get(), anchor, false);
    EXPECT_EQ(m_popup->pos(), QPoint(anchor.left(), anchor.top() - m_popup->height() - 1));
}

TEST_F(PlaceNearTest, FlipsBelowWhenTheScreenEndsAboveTheAnchor) {
    const QRect anchor(m_area.left() + 50, m_area.top() + 10, 30, 16);
    lang::placeNear(m_popup.get(), anchor, true);
    EXPECT_EQ(m_popup->pos(), QPoint(anchor.left(), anchor.bottom() + 2));
}

TEST_F(PlaceNearTest, StaysInsideTheScreenHorizontally) {
    const QRect nearRight(m_area.right() - 20, m_area.top() + 40, 10, 16);
    lang::placeNear(m_popup.get(), nearRight, false);
    EXPECT_EQ(m_popup->x(), m_area.right() - m_popup->width());
    EXPECT_LE(m_popup->geometry().right(), m_area.right());
    // An anchor whose centre is still on the screen but starts left of it is clamped to the edge.
    const QRect pastLeft(m_area.left() - 10, m_area.top() + 40, 40, 16);
    lang::placeNear(m_popup.get(), pastLeft, false);
    EXPECT_EQ(m_popup->x(), m_area.left());
}

} // namespace
