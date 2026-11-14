// SPDX-License-Identifier: Apache-2.0
//
// The EditorView test harness (offscreen): a Document, a simulated scheduler and clock, the
// standalone key routing over a simulated layout, and helpers that send synthesized key events.
#pragma once

#include "frost/input/keymap.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/syntax/language.hpp"
#include "frost/text/document.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/ui/editor/editor_key_routing.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/fonts.hpp"

#include "support/appearance_fixture.hpp"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

namespace frost::ui::test {

// One registry for the test process with the C++ queries compiled (a leaked singleton, so no
// session outlives it).
inline const syntax::LanguageRegistry& languages() {
    static const syntax::LanguageRegistry* registry = [] {
        auto* created = new syntax::LanguageRegistry();
        EXPECT_TRUE(created->prepare(syntax::LanguageId::Cpp));
        EXPECT_TRUE(created->prepare(syntax::LanguageId::Lua));
        return created;
    }();
    return *registry;
}

// Registers the bundled JetBrains Mono once, so every view measures the same monospace font on
// every machine (an offscreen process may have no system fonts).
inline void ensureEditorFonts() {
    static const bool registered = [] {
        const Result<std::string> fontsDir = io::joinPath(resourceDir(), "fonts");
        const Result<std::string> dir = fontsDir ? io::joinPath(*fontsDir, "jetbrains-mono")
                                                 : Result<std::string>(std::unexpected(fontsDir.error()));
        EXPECT_TRUE(dir.has_value());
        if (!dir) {
            return false;
        }
        const auto fonts = registerBundledFonts(io::realFileSystem(), *dir, FontSet::Editor);
        EXPECT_TRUE(fonts.has_value());
        return fonts.has_value();
    }();
    (void)registered;
}

struct HarnessOptions {
    std::string path = "notes.txt";
    input::SimulatedLayoutId layout = input::SimulatedLayoutId::EnUs;
    bool syntax = false;
    syntax::SyntaxOptions syntaxOptions{};
    AppearanceManager* appearance = nullptr;
    EditorViewSettings settings{};
    // The renderer rule's injected environment and fault hooks (ADR 0043).
    EditorRendererHooks renderer{};
    // False: the view is created but not shown (so nothing paints until the test shows it).
    bool show = true;
};

/*
 * A shown (offscreen; options.show) 800 x 600 EditorView over `text`. Members in dependency order: the view is
 * destroyed first, then the routing, the document and the executor.
 */
struct EditorHarness {
    explicit EditorHarness(std::string_view text, const HarnessOptions& options = {})
        : document(text::TextBuffer::fromUtf8(std::string(text)).value()) {
        ensureEditorFonts();
        auto routing = EditorKeyRouting::createStandalone(
            input::simulatedLayout(options.layout), input::KeymapPlatform::Windows, input::KeyPlatform::Windows, sim);
        EXPECT_TRUE(routing.has_value());
        if (routing) {
            keys = std::move(*routing);
        }
        EditorServices services;
        services.keys = keys.get();
        services.clock = &sim;
        services.appearance = options.appearance;
        services.renderer = options.renderer;
        if (options.syntax) {
            services.languages = &languages();
            services.scheduler = &sim;
            services.syntax = options.syntaxOptions;
        }
        view = createEditorView(document, options.path, services, options.settings);
        view->resize(800, 600);
        if (options.show) {
            view->show();
        }
        settle();
    }

    // Runs the simulated workers and collects their parses, then delivers posted events.
    void settle() {
        sim.runUntilIdle();
        (void)view->pollSyntax();
        QCoreApplication::processEvents();
    }

    // Sends a press and a release of `key` to the view.
    void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {}) {
        QKeyEvent down(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(view.get(), &down);
        QKeyEvent up(QEvent::KeyRelease, key, modifiers, text);
        QCoreApplication::sendEvent(view.get(), &up);
    }

    // Types ASCII `text` one key at a time (letters, digits, punctuation, space).
    void type(std::string_view text) {
        for (const char c : text) {
            const QChar ch = QChar::fromLatin1(c);
            press(ch.toUpper().unicode(), ch.isUpper() ? Qt::ShiftModifier : Qt::NoModifier, QString(ch));
        }
    }

    [[nodiscard]] std::string text() const { return document.text().text(); }
    [[nodiscard]] std::uint64_t caret() const { return view->editor().state().primary().head.value(); }

    sched::SimExecutor sim;
    text::Document document;
    std::unique_ptr<EditorKeyRouting> keys;
    std::unique_ptr<EditorView> view;
};

// Skips a test when the process is not on the offscreen platform (a shown widget must never
// reach the user's desktop).
#define FROST_REQUIRE_OFFSCREEN()                                                                                      \
    do {                                                                                                               \
        if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {                                          \
            GTEST_SKIP() << "needs QT_QPA_PLATFORM=offscreen";                                                         \
        }                                                                                                              \
    } while (false)

} // namespace frost::ui::test
