<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0035: The main-window shell (m1/shell-frame)

- Status: accepted
- Date: 2026-11-02
- Plan reference: PLAN.md rows 55, 219, 239-242; ADR 0015 (appearance), ADR 0016 (input), ADR 0031 (feature-first)

## Context
M1 needs the Rider-style window that later slices fill: W1 docks the Solution Explorer (Alt+1) and the build
output and problems windows (Alt+4, Alt+6); m1/editor-view supplies the real editor; m1/settings persists the
layout. Neither m1/settings nor the editor branches are merged yet, so the shell has to stand on seams.

## Decision
1. **Library.** The shell is `frost::ui_shell` in `src/ui/shell` (layer ui, Qt Widgets). `frost::app` only composes
   it: `AppServices` owns the registry, the keymap service, the settings seams and the editor factory, and
   `MainWindow` derives from `ui::ShellWindow`. `ShellWindow::teardown()` drops the dispatcher, the editors and the
   tool window contents while the services still exist; owners of services call it first in their destructors.
2. **Actions and keys.** Menus, the toolbar and shortcuts all go through `frost::input`. Menu items show
   "Name\tShortcut" from `KeymapService::primaryShortcut` and `toDisplayText` (so de-DE shows Ctrl+- for Comment
   Line); QAction shortcuts are never set. `ShortcutDispatcher`, an application event filter, is the only key path:
   it accepts ShortcutOverride for bound keys, feeds each press and release to the matcher once (Qt's re-delivery
   of an ignored event to the parents is skipped), and leaves keys alone while a Qt menu popup is open. The shell
   registers only its own actions; a menu item whose action nobody registered is hidden, one without a handler or
   disabled by its rule is greyed out, and a menu with nothing visible is hidden. Items refresh on aboutToShow and
   when the registry's generation moves.
3. **Seams.** `IShellSettings` (with `InMemoryShellSettings`) stores toolbar/status-bar visibility and each tool
   window's visibility and size; m1/settings binds it to the settings registry later. `IEditorFactory` and
   `IEditorView` are what the tabs and the status bar use; `PlaceholderEditorFactory` opens files read-only through
   `text::loadTextFile` until m1/editor-view.
4. **Tool windows v1.** Fixed sides (left, right, bottom) with stripes; one visible window per side; splitter sizes
   are read back and stored. Drag docking, floating and split modes are later work.
5. **Popups and notifications.** `Popup` is a frameless Qt::Popup frame owned by the window (keyboard-first:
   Up/Down/PgUp/PgDn/Enter/Escape, Escape cancels); `ListPopup` and `InputPopup` build Go to Line, the Ctrl+Tab
   switcher and Quick Switch Theme, and Find Action (m1/shell-nav) reuses them. Balloons are child widgets of the
   central widget, never top-level windows; errors are sticky. Background tasks are thread-safe handles with a
   stop_token; the status bar shows the first task with progress and a cancel button.
6. **i18n.** User-visible strings go through `frost::i18n::tr(context, source)` in core (an installable
   `ITranslator`, plus pseudo-localization that brackets, accents and pads text but keeps placeholders, mnemonics
   and the shortcut column); `qtr` wraps it for Qt. Key and modifier names in `toDisplayText` use the context
   "frost::input::keys". Qt's own translator files are not used yet.
7. **Start-up.** A failed appearance install no longer stops the IDE: the window opens on Qt's style with an error
   notification, and install warnings become one notification.

## Consequences
- Later slices register actions and fill toolbar slots (`MainToolbar::kBuild` and the run configuration combo)
  and tool window contents (`ToolWindowManager::setContent`) without touching the shell.
- Deferred: frameless title bar and snap layouts, drag docking, translation catalogues, the encoding and line
  separator popups behind the status bar widgets (ADR 0018), and the hardening items listed in docs/progress.md.
