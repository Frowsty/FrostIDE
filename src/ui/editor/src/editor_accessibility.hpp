// SPDX-License-Identifier: Apache-2.0
// Installs the custom editor's GUI-thread Qt accessibility interfaces and change notifications.
#pragma once
namespace frost::ui {
class EditorView;
// Called once per fully constructed view; owns no editor/document and installs a process factory once.
void initializeEditorAccessibility(EditorView& view);
}
