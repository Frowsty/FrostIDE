// SPDX-License-Identifier: Apache-2.0
//
// The built-in Visual Studio 2022 and ReSharper keymaps (UX-21; PLAN.md row 227), embedded as keymap
// format v1 text in raw string literals, parsed by builtinKeymap() on each call (keymap.hpp).
/*
 * Sources (clean room, public documentation only; no Visual Studio, ReSharper or Rider keymap file,
 * settings export or source was consulted):
 *   - "visual-studio-2022": Microsoft Learn, "Keyboard shortcuts in Visual Studio" (the default
 *     keyboard shortcuts page for Visual Studio 2022, the "Popular" and "Global" tables and the Text
 *     Editor context table, revision of 2026-10-14). Only a shortcut the page lists for Visual Studio
 *     2022 is used (one marked "Visual Studio 2019 and earlier" is not), and only when a FrostIDE
 *     action does what the documented command does; commands FrostIDE does not have are left out.
 *     Where a command differs a little from FrostIDE's action, the line says so.
 *   - "resharper": JetBrains, ReSharper Help, "Keyboard shortcuts" reference, the "Visual Studio"
 *     scheme column. ReSharper runs inside Visual Studio, so the keymap is the Visual Studio 2022
 *     one with ReSharper's documented shortcuts on top: where ReSharper takes a Visual Studio
 *     shortcut (Alt+Up/Down for members, Ctrl+/ for Document entity, Ctrl+E, T for the Stack Trace
 *     Explorer), the Visual Studio binding is dropped. "Control+R R" in that reference is the
 *     sequence Ctrl+R, R.
 * Both keymaps bind only actions FrostIDE registers, or ids UX-3 owns (tests/input/
 * keymap_coverage_test.cpp). Neither is the default (keymap.id stays "intellij"); the user picks one
 * in Settings > Keymap.
 */

#include <string_view>

namespace frost::input::detail {

namespace {

constexpr std::string_view kVisualStudio2022Keymap =
    R"keymap(# FrostIDE built-in keymap: Visual Studio 2022 defaults (Microsoft Learn, UX-21).
frost-keymap 1
id visual-studio-2022
name Visual Studio 2022

[default]
# Caret movement and selection (Text Editor: Char, Word, Line, Document, Page, View, Scroll line).
editor.moveLeft = Left
editor.moveRight = Right
editor.moveUp = Up
editor.moveDown = Down
editor.moveLeftWithSelection = Shift+Left
editor.moveRightWithSelection = Shift+Right
editor.moveUpWithSelection = Shift+Up
editor.moveDownWithSelection = Shift+Down
editor.wordLeft = Ctrl+Left
editor.wordRight = Ctrl+Right
editor.wordLeftWithSelection = Ctrl+Shift+Left
editor.wordRightWithSelection = Ctrl+Shift+Right
editor.lineStart = Home
editor.lineEnd = End
editor.lineStartWithSelection = Shift+Home
editor.lineEndWithSelection = Shift+End
editor.textStart = Ctrl+Home
editor.textEnd = Ctrl+End
editor.textStartWithSelection = Ctrl+Shift+Home
editor.textEndWithSelection = Ctrl+Shift+End
editor.pageUp = PageUp
editor.pageDown = PageDown
editor.pageUpWithSelection = Shift+PageUp
editor.pageDownWithSelection = Shift+PageDown
editor.pageTop = Ctrl+PageUp
editor.pageBottom = Ctrl+PageDown
editor.scrollUp = Ctrl+Up
editor.scrollDown = Ctrl+Down

# Editing (Edit.*).
editor.backspace = Backspace | Shift+Backspace
editor.delete = Delete
editor.deleteToWordStart = Ctrl+Backspace
editor.deleteToWordEnd = Ctrl+Delete
# Break line is Enter or Shift+Enter; Line open above is Ctrl+Enter, Line open below Ctrl+Shift+Enter.
editor.enter = Enter | Shift+Enter
editor.startNewLineBefore = Ctrl+Enter
editor.startNewLine = Ctrl+Shift+Enter
editor.tab = Tab
editor.unindent = Shift+Tab
editor.escape = Escape
editor.toggleOverwrite = Insert
editor.selectAll = Ctrl+A
editor.copy = Ctrl+C | Ctrl+Insert
editor.cut = Ctrl+X | Shift+Delete
editor.paste = Ctrl+V | Shift+Insert
# Cycle clipboard ring.
editor.pasteFromHistory = Ctrl+Shift+V | Ctrl+Shift+Insert
editor.undo = Ctrl+Z | Alt+Backspace
editor.redo = Ctrl+Y | Ctrl+Shift+Z | Alt+Shift+Backspace
editor.duplicate = Ctrl+D
editor.deleteLine = Ctrl+Shift+L
# Toggle line comment (Ctrl+/, Ctrl+K, Ctrl+/). Comment selection (Ctrl+K, Ctrl+C) and Uncomment
# selection (Ctrl+K, Ctrl+U) run FrostIDE's toggle, which comments or uncomments as they do.
editor.commentLine = Ctrl+Slash | Ctrl+K, Ctrl+Slash | Ctrl+K, Ctrl+C | Ctrl+K, Ctrl+U
# Expand selection and Contract selection.
editor.extendSelection = Alt+Shift+Equal
editor.shrinkSelection = Alt+Shift+Minus
# Move selected lines up and down.
editor.moveLineUp = Alt+Up
editor.moveLineDown = Alt+Down
# Insert next matching caret and Insert carets at all matching.
editor.selectNextOccurrence = Alt+Shift+Period
editor.selectAllOccurrences = Alt+Shift+Semicolon
# Outlining: Collapse and Expand current region, Collapse all outlining, Expand all outlining, Hide selection.
editor.foldCollapse = Ctrl+M, Ctrl+S
editor.foldExpand = Ctrl+M, Ctrl+E
editor.foldCollapseAll = Ctrl+M, Ctrl+A
editor.foldExpandAll = Ctrl+M, Ctrl+X
editor.foldSelection = Ctrl+M, Ctrl+H
editor.find = Ctrl+F
editor.replace = Ctrl+H
editor.findNext = F3
editor.findPrevious = Shift+F3
editor.findWordAtCaret = Ctrl+F3
editor.gotoLine = Ctrl+G
# Goto brace.
navigate.matchingBrace = Ctrl+BracketRight

# Search and navigation.
# Go to all (Ctrl+T, Ctrl+,); Quick launch (Ctrl+Q) searches the IDE's commands.
search.everywhere = Ctrl+T | Ctrl+Comma
search.findAction = Ctrl+Q
search.findInFiles = Ctrl+Shift+F
search.replaceInFiles = Ctrl+Shift+H
# Go to next and previous location (the next error or result in the Error List or Output).
search.nextOccurrence = F8
search.previousOccurrence = Shift+F8
# Go to member.
navigate.fileStructure = Alt+Backslash | Ctrl+Digit1, Ctrl+M
# Quick find symbol.
navigate.gotoSymbol = Alt+Shift+F12
navigate.back = Ctrl+Minus
navigate.forward = Ctrl+Shift+Minus
# Goto next and previous issue in file; View.NextError.
navigate.nextError = Alt+PageDown | Ctrl+Shift+F12
navigate.previousError = Alt+PageUp
# Go to definition (F12) and Go to declaration (Ctrl+F12): FrostIDE's Go to Declaration or Definition.
navigate.declaration = F12 | Ctrl+F12
navigate.findUsages = Shift+F12
navigate.quickDefinition = Alt+F12
navigate.callHierarchy = Ctrl+K, Ctrl+T | Ctrl+K, T | Ctrl+Alt+K
# Toggle header code file.
navigate.relatedFiles = Ctrl+K, Ctrl+O

# Code (IntelliSense).
# Complete word and List members.
code.completion = Ctrl+Space | Ctrl+J
code.parameterInfo = Ctrl+Shift+Space
# Quick info.
code.quickDocumentation = Ctrl+K, Ctrl+I
# Show smart tag (Quick Actions).
code.showIntentions = Ctrl+Period | Alt+Shift+F10
# Insert snippet.
code.insertLiveTemplate = Ctrl+K, Ctrl+X
# Format selection and Format document.
code.reformat = Ctrl+K, Ctrl+F
code.reformatFile = Ctrl+K, Ctrl+D
# Remove and sort usings (FrostIDE organises the file's includes or imports).
code.optimizeImports = Ctrl+R, Ctrl+G

# Refactoring.
refactor.rename = Ctrl+R, Ctrl+R
refactor.extractMethod = Ctrl+R, Ctrl+M

# Tool windows and tabs.
window.project = Ctrl+Alt+L
window.problems = Ctrl+Backslash, Ctrl+E | Ctrl+Backslash, E
window.structure = Ctrl+Alt+T
window.find = Ctrl+Alt+F12
window.todo = Ctrl+Backslash, Ctrl+T | Ctrl+Backslash, T
window.unitTests = Ctrl+E, T
bookmarks.show = Ctrl+K, Ctrl+W
# Close tool window; Activate document window.
window.hideActive = Shift+Escape
window.focusEditor = Escape
window.closeTab = Ctrl+F4 | Ctrl+W
window.nextTab = Ctrl+Alt+PageDown
window.previousTab = Ctrl+Alt+PageUp
window.switcher = Ctrl+Tab
window.switcherBack = Ctrl+Shift+Tab

# Files.
file.new = Ctrl+N
file.newProject = Ctrl+Shift+N
file.openFile = Ctrl+O
# Save selected items and Save all.
file.saveDocument = Ctrl+S
file.saveAll = Ctrl+Shift+S

# Git (Team.Git): Go to git branches, changes and commits.
vcs.branches = Ctrl+Digit0, Ctrl+N | Ctrl+Digit0, N
window.commit = Ctrl+Digit0, Ctrl+G | Ctrl+Digit0, G
vcs.log = Ctrl+Digit0, Ctrl+O | Ctrl+Digit0, O

# Build, run and debug.
build.buildSolution = Ctrl+Shift+B
build.cancelBuild = Ctrl+Pause
run.debug = F5
debug.resume = F5
run.runWithoutDebugging = Ctrl+F5
run.stop = Shift+F5
run.rerun = Ctrl+Shift+F5
debug.toggleBreakpoint = F9
debug.stepOver = F10
debug.stepInto = F11
debug.stepOut = Shift+F11
# Step into specific.
debug.smartStepInto = Alt+Shift+F11
debug.runToCursor = Ctrl+F10
debug.setNextStatement = Ctrl+Shift+F10
# Quick watch.
debug.evaluate = Shift+F9 | Ctrl+Alt+Q
# Break all.
debug.pause = Ctrl+Alt+Pause
debug.attachToProcess = Ctrl+Alt+P
# The Breakpoints window; Breakpoint conditions (Alt+F9, C) edits the breakpoint at the caret.
debug.viewBreakpoints = Ctrl+Alt+B
debug.editBreakpoint = Alt+F9, C

# Bookmarks.
bookmarks.toggle = Ctrl+K, Ctrl+K
bookmarks.next = Ctrl+K, Ctrl+N
bookmarks.previous = Ctrl+K, Ctrl+P

# Agent (PLAN.md section 5.1): Visual Studio has none.
agent.terminal =
)keymap";

constexpr std::string_view kReSharperKeymap =
    R"keymap(# FrostIDE built-in keymap: ReSharper, Visual Studio scheme (JetBrains ReSharper Help; UX-21).
frost-keymap 1
id resharper
name ReSharper (Visual Studio scheme)

[default]
# Caret movement and selection: Visual Studio 2022's.
editor.moveLeft = Left
editor.moveRight = Right
editor.moveUp = Up
editor.moveDown = Down
editor.moveLeftWithSelection = Shift+Left
editor.moveRightWithSelection = Shift+Right
editor.moveUpWithSelection = Shift+Up
editor.moveDownWithSelection = Shift+Down
editor.wordLeft = Ctrl+Left
editor.wordRight = Ctrl+Right
editor.wordLeftWithSelection = Ctrl+Shift+Left
editor.wordRightWithSelection = Ctrl+Shift+Right
editor.lineStart = Home
editor.lineEnd = End
editor.lineStartWithSelection = Shift+Home
editor.lineEndWithSelection = Shift+End
editor.textStart = Ctrl+Home
editor.textEnd = Ctrl+End
editor.textStartWithSelection = Ctrl+Shift+Home
editor.textEndWithSelection = Ctrl+Shift+End
editor.pageUp = PageUp
editor.pageDown = PageDown
editor.pageUpWithSelection = Shift+PageUp
editor.pageDownWithSelection = Shift+PageDown
editor.pageTop = Ctrl+PageUp
editor.pageBottom = Ctrl+PageDown
editor.scrollUp = Ctrl+Up
editor.scrollDown = Ctrl+Down

# Editing: Visual Studio 2022's, with ReSharper's Coding Assistance shortcuts.
editor.backspace = Backspace | Shift+Backspace
editor.delete = Delete
editor.deleteToWordStart = Ctrl+Backspace
editor.deleteToWordEnd = Ctrl+Delete
editor.enter = Enter | Shift+Enter
editor.startNewLineBefore = Ctrl+Enter
editor.tab = Tab
editor.unindent = Shift+Tab
editor.escape = Escape
editor.toggleOverwrite = Insert
editor.selectAll = Ctrl+A
editor.copy = Ctrl+C | Ctrl+Insert
editor.cut = Ctrl+X | Shift+Delete
editor.paste = Ctrl+V | Shift+Insert
# Paste from clipboard history.
editor.pasteFromHistory = Ctrl+Shift+V
editor.undo = Ctrl+Z | Alt+Backspace
editor.redo = Ctrl+Y | Ctrl+Shift+Z | Alt+Shift+Backspace
# Duplicate current line or selection; Join lines.
editor.duplicate = Ctrl+D
editor.joinLines = Ctrl+Shift+J
editor.deleteLine = Ctrl+Shift+L
# Comment with line comment (Ctrl+Alt+/) and Visual Studio's comment commands; ReSharper gives Ctrl+/
# to Document entity, which FrostIDE does not have.
editor.commentLine = Ctrl+Alt+Slash | Ctrl+K, Ctrl+Slash | Ctrl+K, Ctrl+C | Ctrl+K, Ctrl+U
editor.commentBlock = Ctrl+Shift+Slash
# Extend selection and Shrink selection (and Visual Studio's Expand and Contract selection).
editor.extendSelection = Ctrl+Alt+Right | Alt+Shift+Equal
editor.shrinkSelection = Ctrl+Alt+Left | Alt+Shift+Minus
# Move code up and down.
editor.moveStatementUp = Ctrl+Alt+Shift+Up
editor.moveStatementDown = Ctrl+Alt+Shift+Down
editor.selectNextOccurrence = Alt+Shift+Period
editor.selectAllOccurrences = Alt+Shift+Semicolon
editor.foldCollapse = Ctrl+M, Ctrl+S
editor.foldExpand = Ctrl+M, Ctrl+E
editor.foldCollapseAll = Ctrl+M, Ctrl+A
editor.foldExpandAll = Ctrl+M, Ctrl+X
editor.foldSelection = Ctrl+M, Ctrl+H
editor.find = Ctrl+F
editor.replace = Ctrl+H
editor.findNext = F3
editor.findPrevious = Shift+F3
editor.findWordAtCaret = Ctrl+F3
editor.gotoLine = Ctrl+G
navigate.matchingBrace = Ctrl+BracketRight

# Navigation and Search.
# Search Everywhere/Go to Type; Go to Action (and Visual Studio's Quick launch).
search.everywhere = Ctrl+T
search.findAction = Ctrl+Shift+A | Ctrl+Q
search.findInFiles = Ctrl+Shift+F
search.replaceInFiles = Ctrl+Shift+H
# Go to previous and next location/highlight; Visual Studio's next and previous location.
search.nextOccurrence = Ctrl+Alt+PageDown | F8
search.previousOccurrence = Ctrl+Alt+PageUp | Shift+F8
navigate.gotoFile = Ctrl+Shift+T
# Go to file member.
navigate.fileStructure = Alt+Backslash
navigate.gotoSymbol = Alt+Shift+T
# Go to type of symbol.
navigate.typeDeclaration = Ctrl+Shift+F11
# Go to declaration (F12) and Go to implementation (Ctrl+F12).
navigate.declaration = F12
navigate.implementation = Ctrl+F12
# Switch header/source (C++) and Go to related files.
navigate.relatedFiles = Alt+O | Ctrl+Alt+F7
# Go to base symbols.
navigate.superMethod = Alt+Home
# Go to usage.
navigate.showUsages = Alt+Shift+F12
# Go to next and previous member.
navigate.nextMember = Alt+Down
navigate.previousMember = Alt+Up
navigate.findUsages = Shift+F12
# Highlight usages in file.
navigate.highlightUsages = Alt+Shift+F11
# View recent files; View recent edits; Go to previous edit.
navigate.recentFiles = Ctrl+Comma
navigate.recentLocations = Ctrl+Shift+Comma
navigate.lastEditLocation = Ctrl+Shift+Backspace
# Next and previous code issue in file; next and previous error/warning in the solution.
navigate.nextError = Alt+PageDown | Alt+Shift+PageDown
navigate.previousError = Alt+PageUp | Alt+Shift+PageUp
navigate.back = Ctrl+Minus
navigate.forward = Ctrl+Shift+Minus
navigate.quickDefinition = Alt+F12
navigate.callHierarchy = Ctrl+K, Ctrl+T | Ctrl+K, T | Ctrl+Alt+K
# View hierarchy of types.
navigate.typeHierarchy = Ctrl+E, H

# Coding Assistance.
# Show action list (and Visual Studio's Quick Actions).
code.showIntentions = Alt+Enter | Ctrl+Period
code.completion = Ctrl+Space
code.smartCompletion = Ctrl+Alt+Space
code.completeStatement = Ctrl+Shift+Enter
code.parameterInfo = Ctrl+Shift+Space
code.quickDocumentation = Ctrl+Shift+F1 | Ctrl+K, Ctrl+I
# Reformat code (and Visual Studio's Format selection and Format document).
code.reformat = Ctrl+Alt+Enter | Ctrl+K, Ctrl+F
code.reformatFile = Ctrl+K, Ctrl+D
# Insert live template; Surround with template; Generate code.
code.insertLiveTemplate = Ctrl+E, L | Ctrl+K, Ctrl+X
code.surroundWith = Ctrl+E, U
code.generate = Alt+Insert
code.optimizeImports = Ctrl+R, Ctrl+G

# Refactorings ("Control+R R" is Ctrl+R, R; Visual Studio's Ctrl+R, Ctrl+R and Ctrl+R, Ctrl+M stay).
refactor.this = Ctrl+Shift+R
refactor.rename = Ctrl+R, R | Ctrl+R, Ctrl+R
refactor.move = Ctrl+R, O
refactor.safeDelete = Ctrl+R, D
refactor.extractMethod = Ctrl+R, M | Ctrl+R, Ctrl+M
refactor.extractVariable = Ctrl+R, V
refactor.extractField = Ctrl+R, F
refactor.extractParameter = Ctrl+R, P
refactor.inline = Ctrl+R, I
refactor.changeSignature = Ctrl+R, S

# Tool windows and tabs.
window.project = Ctrl+Alt+L
window.problems = Ctrl+Backslash, Ctrl+E | Ctrl+Backslash, E
# File structure; Find Results window; To-do items; Unit Test Explorer.
window.structure = Ctrl+Alt+F
window.find = Ctrl+Alt+F12
window.todo = Ctrl+Alt+D
window.unitTests = Ctrl+Alt+U
# View bookmarks (and Visual Studio's Bookmark window).
bookmarks.show = Ctrl+Backquote | Ctrl+K, Ctrl+W
window.hideActive = Shift+Escape
window.focusEditor = Escape
window.closeTab = Ctrl+F4 | Ctrl+W
window.switcher = Ctrl+Tab
window.switcherBack = Ctrl+Shift+Tab
# Run configurations popup.
run.selectConfiguration = Ctrl+Alt+Shift+R

# Files.
file.new = Ctrl+N
file.newProject = Ctrl+Shift+N
file.openFile = Ctrl+O
file.saveDocument = Ctrl+S
file.saveAll = Ctrl+Shift+S

# Git (Visual Studio's Team.Git).
vcs.branches = Ctrl+Digit0, Ctrl+N | Ctrl+Digit0, N
window.commit = Ctrl+Digit0, Ctrl+G | Ctrl+Digit0, G
vcs.log = Ctrl+Digit0, Ctrl+O | Ctrl+Digit0, O

# Build, run and debug: Visual Studio 2022's; Go to breakpoints is ReSharper's.
build.buildSolution = Ctrl+Shift+B
build.cancelBuild = Ctrl+Pause
run.debug = F5
debug.resume = F5
run.runWithoutDebugging = Ctrl+F5
run.stop = Shift+F5
run.rerun = Ctrl+Shift+F5
debug.toggleBreakpoint = F9
debug.stepOver = F10
debug.stepInto = F11
debug.stepOut = Shift+F11
# Visual Studio's Step into specific (Alt+Shift+F11) is ReSharper's Highlight usages in file here.
debug.runToCursor = Ctrl+F10
debug.setNextStatement = Ctrl+Shift+F10
debug.evaluate = Shift+F9 | Ctrl+Alt+Q
debug.pause = Ctrl+Alt+Pause
debug.attachToProcess = Ctrl+Alt+P
debug.viewBreakpoints = Ctrl+Alt+F9 | Ctrl+Alt+B
debug.editBreakpoint = Alt+F9, C

# Bookmarks: Visual Studio's, and ReSharper's numbered bookmarks (Ctrl+[digit], Ctrl+Shift+[digit]).
bookmarks.toggle = Ctrl+K, Ctrl+K
bookmarks.next = Ctrl+K, Ctrl+N
bookmarks.previous = Ctrl+K, Ctrl+P
bookmarks.goto1 = Ctrl+Digit1
bookmarks.goto2 = Ctrl+Digit2
bookmarks.goto3 = Ctrl+Digit3
bookmarks.goto4 = Ctrl+Digit4
bookmarks.goto5 = Ctrl+Digit5
bookmarks.goto6 = Ctrl+Digit6
bookmarks.goto7 = Ctrl+Digit7
bookmarks.goto8 = Ctrl+Digit8
bookmarks.goto9 = Ctrl+Digit9
bookmarks.toggle1 = Ctrl+Shift+Digit1
bookmarks.toggle2 = Ctrl+Shift+Digit2
bookmarks.toggle3 = Ctrl+Shift+Digit3
bookmarks.toggle4 = Ctrl+Shift+Digit4
bookmarks.toggle5 = Ctrl+Shift+Digit5
bookmarks.toggle6 = Ctrl+Shift+Digit6
bookmarks.toggle7 = Ctrl+Shift+Digit7
bookmarks.toggle8 = Ctrl+Shift+Digit8
bookmarks.toggle9 = Ctrl+Shift+Digit9

# Agent (PLAN.md section 5.1): neither Visual Studio nor ReSharper has one.
agent.terminal =
)keymap";

} // namespace

// Read by builtinKeymapText (keymap_intellij.cpp). Valid for the program's lifetime.
std::string_view visualStudio2022KeymapText() noexcept { return kVisualStudio2022Keymap; }
std::string_view reSharperKeymapText() noexcept { return kReSharperKeymap; }

} // namespace frost::input::detail
