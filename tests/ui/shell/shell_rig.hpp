// SPDX-License-Identifier: Apache-2.0
//
// Test support for the shell tests (tests/ui/shell): a fake editor and its factory, and ShellRig,
// the services a ShellWindow needs with a simulated keyboard layout, so menus and shortcuts do not
// depend on the machine's layout.
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/ui/shell/editor_host.hpp"
#include "frost/ui/shell/shell_settings.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include <QApplication>
#include <QDeadlineTimer>
#include <QLabel>
#include <QWidget>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frost::ui::test {

// An editor whose status the test sets; it records Go to Line calls.
class FakeEditor final : public IEditorView {
public:
    FakeEditor(std::string path, IEditorHost& host) : m_path(std::move(path)), m_host(host), m_widget(new QLabel()) {
        m_widget->setObjectName(QStringLiteral("fakeEditor"));
        m_widget->setFocusPolicy(Qt::StrongFocus);
        m_widget->setText(QString::fromStdString(m_path));
    }
    FakeEditor(const FakeEditor&) = delete ("an editor view has identity");
    FakeEditor& operator=(const FakeEditor&) = delete ("an editor view has identity");
    ~FakeEditor() override { delete m_widget; }

    [[nodiscard]] QWidget* widget() override { return m_widget; }
    [[nodiscard]] const std::string& path() const override { return m_path; }
    [[nodiscard]] EditorStatus status() const override { return m_status; }
    void goToLine(int line, int column) override {
        m_status.line = line;
        m_status.column = column;
        gotoCalls.emplace_back(line, column);
        m_host.editorStatusChanged(*this);
    }
    [[nodiscard]] bool requestClose() override { return allowClose; }

    // Replaces the status and tells the host.
    void setStatus(EditorStatus status) {
        m_status = std::move(status);
        m_host.editorStatusChanged(*this);
    }
    void setDirty(bool dirty) {
        EditorStatus next = m_status;
        next.dirty = dirty;
        setStatus(next);
    }

    bool allowClose = true;
    std::vector<std::pair<int, int>> gotoCalls;

private:
    std::string m_path;
    IEditorHost& m_host;
    QLabel* m_widget;
    EditorStatus m_status;
};

// Creates FakeEditors; paths in `failing` fail with NotFound.
class FakeEditorFactory final : public IEditorFactory {
public:
    [[nodiscard]] Result<std::unique_ptr<IEditorView>> open(const std::string& path, IEditorHost& host) override {
        if (std::ranges::find(failing, path) != failing.end()) {
            return fail(ErrorCode::NotFound, "no such file: " + path);
        }
        auto editor = std::make_unique<FakeEditor>(path, host);
        last = editor.get();
        ++opened;
        return std::unique_ptr<IEditorView>(std::move(editor));
    }

    std::vector<std::string> failing;
    FakeEditor* last = nullptr;
    int opened = 0;
};

// The services of one test window. Declare it before the window.
struct ShellRig {
    explicit ShellRig(input::SimulatedLayoutId layout = input::SimulatedLayoutId::EnUs) {
        auto created = input::KeymapService::create(registry, keymapSettings, input::KeymapPlatform::Windows,
                                                    input::simulatedLayout(layout), std::chrono::milliseconds(400));
        EXPECT_TRUE(created.has_value());
        keymap.emplace(std::move(*created));
    }

    [[nodiscard]] ShellEnvironment environment() {
        ShellEnvironment env{registry, *keymap, settings, editors};
        env.followActiveLayout = false;
        return env;
    }

    // Registers `id` with `name` and a handler that counts its runs.
    input::ActionId addCounted(std::string id, std::string name, int& runs) {
        input::ActionSpec spec;
        spec.id = std::move(id);
        spec.name = std::move(name);
        spec.handler = [&runs](const input::ActionContext&, const input::InvokeInfo&) { ++runs; };
        auto added = registry.add(std::move(spec));
        EXPECT_TRUE(added.has_value());
        return added.value_or(input::kNoAction);
    }

    input::ActionRegistry registry;
    input::MemoryKeymapSettings keymapSettings;
    std::optional<input::KeymapService> keymap;
    InMemoryShellSettings settings;
    FakeEditorFactory editors;
};

// Pumps events until `done` or about two seconds pass; true when `done` was reached.
inline bool waitFor(const std::function<bool()>& done) {
    // Returns the evaluation that ended the wait, as tests/app/workbench_rig.hpp does: a second
    // evaluation after the loop could see a state changed in between and report a reached
    // condition as not reached.
    const QDeadlineTimer deadline(2000);
    for (;;) {
        if (done())
            return true;
        if (deadline.hasExpired())
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

// Shows `window` on the offscreen platform and waits until it is exposed.
inline void showAndWait(QWidget& window) {
    window.show();
    waitFor([&window] { return window.isVisible(); });
    QApplication::processEvents();
}

} // namespace frost::ui::test
