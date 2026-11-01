// SPDX-License-Identifier: Apache-2.0
//
// ImeTracker: composition state from constructed Qt events (preedit, commit, cancel,
// focus changes, window deactivation, application state changes), reset() and the
// takeCompositionStarted edge.

#include "frost/ui/ime_tracker.hpp"

#include <QEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QString>
#include <gtest/gtest.h>

namespace frost::ui {
namespace {

QInputMethodEvent preedit(const QString& text) { return QInputMethodEvent(text, {}); }

// Qt events cannot be copied or moved, so the commit is observed in place.
void observeCommit(ImeTracker& ime, const QString& text) {
    QInputMethodEvent event;
    event.setCommitString(text);
    ime.observe(event);
}

TEST(ImeTracker, PreeditCommitAndCancel) {
    ImeTracker ime;
    EXPECT_FALSE(ime.composing());
    EXPECT_FALSE(ime.takeCompositionStarted());

    ime.observe(preedit(QStringLiteral("n")));
    EXPECT_TRUE(ime.composing());
    EXPECT_TRUE(ime.takeCompositionStarted());
    EXPECT_FALSE(ime.takeCompositionStarted()) << "an edge, reported once";

    ime.observe(preedit(QStringLiteral("ni")));
    EXPECT_TRUE(ime.composing());
    EXPECT_FALSE(ime.takeCompositionStarted()) << "a longer preedit is not a new start";

    observeCommit(ime, QString::fromUtf8("\xE4\xBD\xA0"));
    EXPECT_FALSE(ime.composing());

    ime.observe(preedit(QStringLiteral("h")));
    EXPECT_TRUE(ime.takeCompositionStarted()) << "a new composition";
    ime.observe(preedit(QString()));
    EXPECT_FALSE(ime.composing()) << "an empty preedit is a cancel";
}

TEST(ImeTracker, FocusAndWindowChangesEndComposition) {
    for (const QEvent::Type type : {QEvent::FocusIn, QEvent::FocusOut}) {
        ImeTracker ime;
        ime.observe(preedit(QStringLiteral("a")));
        ime.observe(QFocusEvent(type));
        EXPECT_FALSE(ime.composing()) << type;
    }
    ImeTracker ime;
    ime.observe(preedit(QStringLiteral("a")));
    ime.observe(QEvent(QEvent::WindowDeactivate));
    EXPECT_FALSE(ime.composing());
}

TEST(ImeTracker, ApplicationStateChanges) {
    for (const Qt::ApplicationState state :
         {Qt::ApplicationInactive, Qt::ApplicationHidden, Qt::ApplicationSuspended}) {
        ImeTracker ime;
        ime.observe(preedit(QStringLiteral("a")));
        ime.observe(QApplicationStateChangeEvent(state));
        EXPECT_FALSE(ime.composing()) << state;
    }
    ImeTracker ime;
    ime.observe(preedit(QStringLiteral("a")));
    ime.observe(QApplicationStateChangeEvent(Qt::ApplicationActive));
    EXPECT_TRUE(ime.composing()) << "becoming active keeps the composition";
}

TEST(ImeTracker, OtherEventsAreIgnored) {
    ImeTracker ime;
    ime.observe(preedit(QStringLiteral("a")));
    for (const QEvent::Type type : {QEvent::Timer, QEvent::WindowActivate, QEvent::Show, QEvent::UpdateRequest}) {
        ime.observe(QEvent(type));
        EXPECT_TRUE(ime.composing()) << type;
    }
}

TEST(ImeTracker, Reset) {
    ImeTracker ime;
    ime.observe(preedit(QStringLiteral("a")));
    ime.reset();
    EXPECT_FALSE(ime.composing());
    EXPECT_FALSE(ime.takeCompositionStarted()) << "reset clears the pending edge";
    ime.observe(preedit(QStringLiteral("b")));
    EXPECT_TRUE(ime.takeCompositionStarted());
}

} // namespace
} // namespace frost::ui
