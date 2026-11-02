// SPDX-License-Identifier: Apache-2.0
//
// GoToLinePopup (ADR 0037; feature row 94): the small "line[:column]" input Ctrl+G opens over an
// EditorView. A child frame of the editor, not a top-level window; EditorView parses the input with
// frost::text::parseGoToLine and runs the goToLine command.
#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QLineEdit;

namespace frost::ui {

/*
 * A label, the input field and an error line. Enter emits accepted(text), Escape cancelled().
 * English labels (TODO(M1): tr() with m1/shell-frame's frost/ui/tr.hpp). GUI thread only.
 */
class GoToLinePopup final : public QFrame {
    Q_OBJECT

public:
    explicit GoToLinePopup(QWidget* parent = nullptr);

    // Sets the field to `text`, selected, clears the error and focuses the field.
    void open(const QString& text);
    void showError(const QString& message);
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QLineEdit* field() const noexcept { return m_field; }

signals:
    void accepted(const QString& text);
    void cancelled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* m_field;
    QLabel* m_error;
};

} // namespace frost::ui
