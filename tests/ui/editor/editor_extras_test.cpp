// SPDX-License-Identifier: Apache-2.0
// UX-5/7: preview resource denial and bookmark identity through document edits and Undo.
#include "frost/ui/editor/markdown_preview.hpp"
#include "editor/editor_harness.hpp"
#include <QTextDocument>
#include <QUrl>
#include <gtest/gtest.h>
namespace frost::ui {
TEST(EditorExtras, MarkdownNeverLoadsEmbeddedFilesOrRemoteImages) {
    MarkdownPreview preview;
    ASSERT_TRUE(preview.setSourceMarkdown(QStringLiteral("# Heading\n![secret](file:///private/secret.png)\n![remote](https://example.invalid/track.png)\n<script>alert(1)</script>")));
    EXPECT_FALSE(preview.document()->resource(QTextDocument::ImageResource, QUrl("file:///private/secret.png")).isValid());
    EXPECT_FALSE(preview.document()->resource(QTextDocument::ImageResource, QUrl("https://example.invalid/track.png")).isValid());
    EXPECT_FALSE(preview.openExternalLinks());
    EXPECT_FALSE(preview.openLinks());
    EXPECT_FALSE(preview.setSourceMarkdown(QString(512 * 1024 + 1, QLatin1Char('x'))));
}
TEST(EditorExtras, BookmarkFollowsInsertedLinesAndUndo) {
    test::EditorHarness h("first\nsecond\nthird\n");
    h.view->goToLine(2, 1);
    h.view->toggleBookmark();
    ASSERT_EQ(h.view->bookmarks().size(), 1u);
    EXPECT_EQ(h.view->bookmarks()[0], LineIndex{1});
    h.view->goToLine(1, 1);
    ASSERT_TRUE(h.view->typeText("new\n"));
    ASSERT_EQ(h.view->bookmarks().size(), 1u);
    EXPECT_EQ(h.view->bookmarks()[0], LineIndex{2});
    EXPECT_TRUE(h.view->navigateBookmark(true));
    EXPECT_EQ(h.view->status().line, 3);
    ASSERT_TRUE(h.view->perform(EditorAction::Undo));
    ASSERT_EQ(h.view->bookmarks().size(), 1u);
    EXPECT_EQ(h.view->bookmarks()[0], LineIndex{1});
}
}
