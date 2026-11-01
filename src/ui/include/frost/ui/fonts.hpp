// SPDX-License-Identifier: Apache-2.0
//
// Registration of the bundled fonts (ADR 0015 decision 9): Inter 4.1 for the UI and JetBrains
// Mono 2.304 for the editor, both OFL-1.1, read from <resources>/fonts through IFileSystem and
// added as application fonts. The files are bundled unmodified, with their licences.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"

#include <QString>
#include <QStringList>

#include <string_view>

namespace frost::ui {

enum class FontSet : std::uint8_t {
    Ui,     // fonts/inter: Inter-Regular, -Medium, -SemiBold, -Bold
    Editor, // fonts/jetbrains-mono: JetBrainsMono-Regular, -Italic, -Bold, -BoldItalic
};

struct RegisteredFonts {
    QStringList families;  // every family Qt reported, deduplicated, in file order
    QString primaryFamily; // the family Qt reports for the Regular file; never a literal
};

/*
 * Reads every file of `set` from `fontsDir` (absolute UTF-8: <resources>/fonts/inter or
 * <resources>/fonts/jetbrains-mono) with readFile and registers it with
 * QFontDatabase::addApplicationFontFromData. Not idempotent: AppearanceManager tracks what is
 * registered (no static cache). Needs a QGuiApplication; GUI thread only.
 * Errors: NotFound, PermissionDenied, Busy or IoError naming the file (nothing is registered
 * after the first failure, but earlier files of the set stay registered); Corrupt when Qt
 * rejects a file. Budget: B1 for FontSet::Ui; the editor set is registered lazily (Budget: B19).
 */
[[nodiscard]] Result<RegisteredFonts> registerBundledFonts(io::IFileSystem& fs, std::string_view fontsDir, FontSet set);

} // namespace frost::ui
