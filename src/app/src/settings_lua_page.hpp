// SPDX-License-Identifier: Apache-2.0
// The Settings dialog's Lua page (LUA-4): the open project's .luarc.json (runtime version,
// libraries, globals, diagnostics switches) edited in place through lang::readLuaRc/writeLuaRc,
// so what the page shows is what LuaLS reads.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/lang/lua_rc.hpp"

#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;

namespace frost::app {

/*
 * GUI thread only; `fs` outlives the page. Without a project (empty root) the page says so and
 * edits nothing. A .luarc.json that cannot be read or parsed (comments, wrong types) is shown as
 * an error and never overwritten. apply() writes only when something changed and refuses when the
 * file changed on disk since load() (Busy); `applied` runs after a write, so the caller can tell
 * the running LuaLS (didChangeConfiguration).
 */
class SettingsLuaPage final : public QWidget {
public:
    SettingsLuaPage(io::IFileSystem& fs, std::string projectRoot, std::function<void()> dirtyChanged,
                    std::function<void()> applied, QWidget* parent = nullptr);

    // Reads the project's .luarc.json into the controls (discarding edits).
    void load();
    [[nodiscard]] bool dirty() const;
    // Writes the edits. Errors: Busy (changed on disk), Corrupt, the file system's; none without
    // edits or without a project.
    [[nodiscard]] Result<void> apply();
    // Whether the page's labels contain `query` (case ignored), for the dialog's search.
    [[nodiscard]] bool matches(const QString& query) const;

private:
    [[nodiscard]] lang::LuaRcSettings current() const;
    void edited();

    io::IFileSystem& m_fs;
    std::string m_root;
    std::function<void()> m_dirtyChanged;
    std::function<void()> m_applied;
    std::optional<lang::LuaRcFile> m_loaded; // nullopt: no project, or the file could not be read.
    bool m_loading = false;
    QLabel* m_path = nullptr;
    QLabel* m_status = nullptr;
    QComboBox* m_version = nullptr;
    QPlainTextEdit* m_libraries = nullptr;
    QPlainTextEdit* m_globals = nullptr;
    QCheckBox* m_diagnostics = nullptr;
    QPlainTextEdit* m_disabled = nullptr;
};

} // namespace frost::app
