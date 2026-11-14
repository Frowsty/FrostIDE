// SPDX-License-Identifier: Apache-2.0
//
// Shared UI test set-up (appearance_fixture.hpp).

#include "support/appearance_fixture.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/frost_style.hpp"

#include <QApplication>
#include <QFont>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace frost::ui::test {

std::string resourceDir() { return FROST_RESOURCE_DIR; }

std::string sourceDir() { return FROST_SOURCE_DIR; }

std::unique_ptr<Installed> install(const AppearanceSettings& settings, io::IFileSystem* fs, std::string resources,
                                   OsAppearance os) {
    auto installed = std::make_unique<Installed>();
    if (auto stored = installed->settings.store(settings); !stored) {
        ADD_FAILURE() << "invalid test settings: " << stored.error().describe();
        return nullptr;
    }
    installed->os.set(os);
    installed->manager = std::make_unique<AppearanceManager>(AppearanceEnvironment{
        fs != nullptr ? *fs : io::realFileSystem(), resources.empty() ? resourceDir() : std::move(resources),
        installed->settings, &installed->os});
    auto app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app == nullptr) {
        ADD_FAILURE() << "no QApplication";
        return nullptr;
    }
    auto report = installed->manager->install(*app);
    if (!report) {
        ADD_FAILURE() << "install failed: " << report.error().describe();
        return nullptr;
    }
    installed->report = std::move(*report);
    return installed;
}

MemResources::~MemResources() {
    if (FrostStyle::current() != nullptr) {
        QApplication::setStyle(QStringLiteral("Fusion"));
    }
}

std::string MemResources::path(std::string_view relative) const {
    std::string result = root;
    std::string_view rest = relative;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        result = io::joinPath(result, part).value_or(result);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    return result;
}

std::unique_ptr<MemResources> mirrorResources(std::initializer_list<std::string_view> skip) {
    auto mem = std::make_unique<MemResources>();
#ifdef _WIN32
    mem->fs.addRoot("C:\\");
    mem->root = "C:\\res";
#else
    mem->root = "/res";
#endif
    const std::string dir = resourceDir();
    const std::filesystem::path base(std::u8string(dir.begin(), dir.end()));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string relative = entry.path().lexically_relative(base).generic_string();
        if (std::ranges::find(skip, std::string_view(relative)) != skip.end()) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const std::string target = mem->path(relative);
        if (auto made = mem->fs.createDirectories(io::parentPath(target)); !made) {
            ADD_FAILURE() << made.error().describe();
            return nullptr;
        }
        if (auto written = mem->fs.writeFileAtomic(target, bytes); !written) {
            ADD_FAILURE() << written.error().describe();
            return nullptr;
        }
    }
    return mem;
}

void applyTestFontSettings() {
    QFont font = QApplication::font();
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    QApplication::setFont(font);
}

} // namespace frost::ui::test
