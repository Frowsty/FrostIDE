// SPDX-License-Identifier: Apache-2.0
//
// registerBundledFonts (fonts.hpp).

#include "frost/ui/fonts.hpp"

#include "frost/io/path.hpp"

#include <QByteArray>
#include <QFontDatabase>

#include <array>
#include <string>

namespace frost::ui {

namespace {

constexpr std::array<std::string_view, 4> kUiFiles{"Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf",
                                                   "Inter-Bold.ttf"};
constexpr std::array<std::string_view, 4> kEditorFiles{"JetBrainsMono-Regular.ttf", "JetBrainsMono-Italic.ttf",
                                                       "JetBrainsMono-Bold.ttf", "JetBrainsMono-BoldItalic.ttf"};

} // namespace

Result<RegisteredFonts> registerBundledFonts(io::IFileSystem& fs, std::string_view fontsDir, FontSet set) {
    // Budget: B1 (Ui). About 1.7 MB read and registered; Qt parses the files lazily.
    const auto& files = set == FontSet::Ui ? kUiFiles : kEditorFiles;
    RegisteredFonts result;
    for (const std::string_view name : files) {
        auto path = io::joinPath(fontsDir, name);
        if (!path) {
            return std::unexpected(std::move(path).error());
        }
        auto bytes = fs.readFile(*path);
        if (!bytes) {
            return std::unexpected(std::move(bytes).error().withContext("while reading font " + *path));
        }
        const int id =
            QFontDatabase::addApplicationFontFromData(QByteArray(bytes->data(), static_cast<qsizetype>(bytes->size())));
        if (id < 0) {
            return fail(ErrorCode::Corrupt, "Qt rejected the font " + *path);
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (result.primaryFamily.isEmpty() && !families.isEmpty()) {
            result.primaryFamily = families.front();
        }
        for (const QString& family : families) {
            if (!result.families.contains(family)) {
                result.families.push_back(family);
            }
        }
    }
    if (result.primaryFamily.isEmpty()) {
        return fail(ErrorCode::Corrupt, "Qt reported no family for the fonts in " + std::string(fontsDir));
    }
    return result;
}

} // namespace frost::ui
