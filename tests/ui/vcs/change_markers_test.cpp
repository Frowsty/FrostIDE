// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of EditorChangeMarkers (frost/ui/vcs/change_markers.hpp) on an offscreen
// EditorView with a fake base provider: the markers against the base, the debounce after typing, a
// stripe click opening the change popup with the old text, Rollback Lines, and files without a base.

#include "frost/ui/editor/display_list.hpp"
#include "frost/ui/vcs/change_markers.hpp"

#include "editor/editor_harness.hpp"
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

using test::EditorHarness;

// Delivers `base` (nullopt: no HEAD version) at once and counts the reads.
struct FakeBase {
    std::optional<std::string> base;
    int reads = 0;

    EditorChangeMarkers::BaseProvider provider() {
        return [this](const std::string&, std::function<void(Result<std::optional<std::string>>)> done) {
            ++reads;
            done(base);
        };
    }
};

constexpr std::string_view kBase = "one\ntwo\nthree\nfour\n";

// Runs the event loop until `done` or two seconds.
template<typename Done>
bool waitFor(Done done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

TEST(EditorChangeMarkers, MarksTheLinesThatDifferFromTheBase) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nTWO\nthree\nfour\nfive\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    EXPECT_EQ(base.reads, 1);
    ASSERT_TRUE(markers.hasBase());
    ASSERT_EQ(markers.changes().size(), 2U);
    EXPECT_EQ(markers.changes()[0].kind, vcs::LineChangeKind::Modified);
    EXPECT_EQ(markers.changes()[1].kind, vcs::LineChangeKind::Added);
    EXPECT_EQ(markers.baseText(0), "two\n");
    EXPECT_EQ(markers.baseText(1), "");
    EXPECT_EQ(h.view->lineChanges(),
              (std::vector<LineChangeMarker>{LineChangeMarker{LineIndex{1}, 1, ChangeStripeKind::Modified},
                                             LineChangeMarker{LineIndex{4}, 1, ChangeStripeKind::Added}}));
    EXPECT_EQ(h.view->buildDisplayList().changeStripes.size(), 2U);
}

TEST(EditorChangeMarkers, RecomputesAfterTypingStops) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h(kBase);
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    markers.setDebounce(std::chrono::milliseconds(10));
    EXPECT_TRUE(markers.changes().empty());
    int updates = 0;
    QObject::connect(&markers, &EditorChangeMarkers::changesUpdated, [&updates] { ++updates; });
    h.type("x");           // "xone".
    EXPECT_EQ(updates, 0); // Not before the debounce.
    ASSERT_TRUE(waitFor([&] { return updates > 0; }));
    ASSERT_EQ(markers.changes().size(), 1U);
    EXPECT_EQ(markers.changes()[0].kind, vcs::LineChangeKind::Modified);
    EXPECT_EQ(markers.changes()[0].current.begin, LineIndex{0});

    // HEAD moved to the edited text (a commit): the next base read clears the markers.
    base.base = h.text();
    markers.refreshBase();
    EXPECT_EQ(base.reads, 2);
    EXPECT_TRUE(markers.changes().empty());
    EXPECT_TRUE(h.view->lineChanges().empty());
}

TEST(EditorChangeMarkers, AStripeClickShowsTheOldTextAndRollsTheLinesBack) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nTWO\nTHREE\nfour\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    ASSERT_EQ(markers.changes().size(), 1U);
    const EditorDisplayList list = h.view->buildDisplayList();
    ASSERT_EQ(list.changeStripes.size(), 2U); // Lines 1 and 2.

    // A click on the stripe opens the popup with the old lines.
    const QPointF center = list.changeStripes[1].rect.center();
    QMouseEvent press(QEvent::MouseButtonPress, center, h.view->viewport()->mapToGlobal(center), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(h.view->viewport(), &press);
    ASSERT_NE(markers.popup(), nullptr);
    EXPECT_TRUE(markers.popup()->isOpen());
    const auto* old = markers.popup()->findChild<QPlainTextEdit*>(QStringLiteral("changePopupBaseText"));
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(old->toPlainText(), QStringLiteral("two\nthree"));
    ASSERT_NE(markers.popupRollbackButton(), nullptr);
    EXPECT_TRUE(markers.popupRollbackButton()->isEnabled());

    // Rollback Lines restores them, as one undoable edit, and closes the popup.
    markers.popupRollbackButton()->click();
    EXPECT_EQ(h.text(), kBase);
    EXPECT_TRUE(markers.changes().empty());
    EXPECT_TRUE(h.view->lineChanges().empty());
    EXPECT_TRUE(markers.popup() == nullptr || !markers.popup()->isOpen());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // The closed popup deletes itself.
    EXPECT_EQ(markers.popup(), nullptr);
    h.press(Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(h.text(), "one\nTWO\nTHREE\nfour\n");
}

TEST(EditorChangeMarkers, PopupCannotRollbackAfterTheDocumentChanges) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nTWO\nthree\nfour\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    ASSERT_EQ(markers.changes().size(), 1U);
    markers.showChangePopup(0, QPoint(0, 0));
    ASSERT_NE(markers.popupRollbackButton(), nullptr);
    h.type("late ");
    const std::string before = h.text();
    markers.popupRollbackButton()->click();
    EXPECT_EQ(h.text(), before);
}

TEST(EditorChangeMarkers, PopupCannotRollbackAfterPendingBaseCompletes) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nTWO\nthree\nfour\n");
    std::function<void(Result<std::optional<std::string>>)> pending;
    int calls = 0;
    EditorChangeMarkers markers(*h.view, [&](const std::string&, auto done) {
        if (++calls == 1)
            done(std::optional<std::string>(kBase));
        else
            pending = std::move(done);
    });
    ASSERT_EQ(markers.changes().size(), 1U);
    markers.refreshBase();
    ASSERT_TRUE(pending);
    markers.showChangePopup(0, QPoint(0, 0));
    ASSERT_NE(markers.popupRollbackButton(), nullptr);
    pending(std::optional<std::string>("different\nTWO\nthree\nfour\n"));
    const std::string before = h.text();
    markers.popupRollbackButton()->setEnabled(true); // Model an already queued activation.
    markers.popupRollbackButton()->click();
    EXPECT_EQ(h.text(), before);
}

TEST(EditorChangeMarkers, PopupCannotRollbackAfterPendingDebounceRecomputes) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nTWO\nthree\nfour\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    markers.setDebounce(std::chrono::milliseconds(10));
    h.type("late ");
    markers.showChangePopup(0, QPoint(0, 0));
    ASSERT_NE(markers.popupRollbackButton(), nullptr);
    int updates = 0;
    QObject::connect(&markers, &EditorChangeMarkers::changesUpdated, [&updates] { ++updates; });
    ASSERT_TRUE(waitFor([&] { return updates > 0; }));
    const std::string before = h.text();
    markers.popupRollbackButton()->setEnabled(true); // Model an already queued activation.
    markers.popupRollbackButton()->click();
    EXPECT_EQ(h.text(), before);
}

TEST(EditorChangeMarkers, RollsBackAddedAndDeletedLines) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nnew\ntwo\nfour\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    ASSERT_EQ(markers.changes().size(), 2U);
    EXPECT_EQ(markers.changes()[0].kind, vcs::LineChangeKind::Added);
    EXPECT_EQ(markers.changes()[1].kind, vcs::LineChangeKind::Deleted);
    EXPECT_EQ(markers.baseText(1), "three\n");
    EXPECT_TRUE(markers.rollback(1));
    EXPECT_EQ(h.text(), "one\nnew\ntwo\nthree\nfour\n");
    ASSERT_EQ(markers.changes().size(), 1U);
    EXPECT_TRUE(markers.rollback(0));
    EXPECT_EQ(h.text(), kBase);
    EXPECT_FALSE(markers.rollback(0)); // No change left.
}

TEST(EditorChangeMarkers, AFileWithoutABaseHasNoMarkersAndReadOnlyEditorsCannotRollBack) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("new file\n");
    FakeBase none;
    EditorChangeMarkers markers(*h.view, none.provider());
    EXPECT_FALSE(markers.hasBase());
    EXPECT_TRUE(markers.changes().empty());
    EXPECT_TRUE(h.view->lineChanges().empty());

    EditorHarness ro("one\nTWO\nthree\nfour\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers readOnly(*ro.view, base.provider());
    EditorFileInfo info;
    info.readOnly = true;
    ro.view->setFileInfo(info);
    ASSERT_EQ(readOnly.changes().size(), 1U);
    EXPECT_FALSE(readOnly.rollback(0));
    readOnly.showChangePopup(0, QPoint(10, 10));
    ASSERT_NE(readOnly.popupRollbackButton(), nullptr);
    EXPECT_FALSE(readOnly.popupRollbackButton()->isEnabled());
    readOnly.popup()->closePopup(Popup::CloseReason::Cancelled);
}

TEST(EditorChangeMarkers, PopupSupportsPreviousNextChangeNavigationAndCopyOriginalText) {
    FROST_REQUIRE_OFFSCREEN();
    EditorHarness h("one\nTWO\nthree\nFOUR\n");
    FakeBase base{std::string(kBase)};
    EditorChangeMarkers markers(*h.view, base.provider());
    ASSERT_EQ(markers.changes().size(), 2U);

    markers.showChangePopup(0, QPoint(10, 10));
    ASSERT_NE(markers.popup(), nullptr);
    ASSERT_NE(markers.popupPreviousButton(), nullptr);
    ASSERT_NE(markers.popupNextButton(), nullptr);
    ASSERT_NE(markers.popupCopyButton(), nullptr);
    EXPECT_FALSE(markers.popupPreviousButton()->isEnabled());
    EXPECT_TRUE(markers.popupNextButton()->isEnabled());
    EXPECT_TRUE(markers.popupCopyButton()->isEnabled());

    markers.popupCopyButton()->click();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("two\n"));

    markers.popupNextButton()->click();
    EXPECT_EQ(h.view->status().line, 4);
    ASSERT_NE(markers.popup(), nullptr);
    EXPECT_TRUE(markers.popupPreviousButton()->isEnabled());
    EXPECT_FALSE(markers.popupNextButton()->isEnabled());

    markers.popupPreviousButton()->click();
    EXPECT_EQ(h.view->status().line, 2);
    markers.popup()->closePopup(Popup::CloseReason::Cancelled);
}

} // namespace
} // namespace frost::ui
