// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/pinned_watches.hpp"

#include "frost/rpc/json.hpp"
#include "frost/settings/document.hpp"
#include "frost/settings/migration.hpp"
#include "frost/settings/project_layout.hpp"

#include <algorithm>
namespace frost::debug {
namespace {
constexpr std::string_view kMember = "pinnedWatches";
Result<settings::SettingsDocument> read(io::IFileSystem& fs, const settings::ProjectLayout& layout) {
    auto exists = fs.exists(layout.personalSettings);
    if (!exists)
        return std::unexpected(exists.error());
    if (!*exists)
        return settings::SettingsDocument::create(settings::kCurrentSchemaVersion);
    auto bytes = fs.readFile(layout.personalSettings);
    if (!bytes)
        return std::unexpected(bytes.error());
    return settings::SettingsDocument::parse(std::move(*bytes));
}
} // namespace
Result<std::vector<std::string>> loadPinnedWatches(io::IFileSystem& fs, std::string_view root) {
    auto layout = settings::ProjectLayout::forRoot(root);
    if (!layout)
        return std::unexpected(layout.error());
    auto document = read(fs, *layout);
    if (!document)
        return std::unexpected(document.error());
    const auto* member = document->find(kMember);
    if (!member)
        return std::vector<std::string>{};
    auto list = rpc::parseJson(document->rawValue(*member));
    if (!list || !list->isArray() || list->size() > 128)
        return fail(ErrorCode::Corrupt, "Pinned watches must be an array of at most 128 expressions");
    std::vector<std::string> result;
    for (const auto& value : list->items()) {
        const auto* expression = value.asString();
        if (!expression || expression->empty() || expression->size() > 4096 ||
            expression->find('\0') != std::string::npos)
            return fail(ErrorCode::Corrupt, "Invalid pinned watch expression");
        if (std::ranges::find(result, *expression) == result.end())
            result.push_back(*expression);
    }
    return result;
}
Result<void> savePinnedWatches(io::IFileSystem& fs, std::string_view root,
                               const std::vector<std::string>& expressions) {
    if (expressions.size() > 128)
        return fail(ErrorCode::OutOfRange, "At most 128 watches can be pinned");
    auto list = rpc::Json::array();
    for (const auto& expression : expressions) {
        if (expression.empty() || expression.size() > 4096 || expression.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "Invalid pinned watch expression");
        list.push(expression);
    }
    auto layout = settings::ProjectLayout::forRoot(root);
    if (!layout)
        return std::unexpected(layout.error());
    auto document = read(fs, *layout);
    if (!document)
        return std::unexpected(document.error());
    if (const auto* old = document->find(kMember); old && document->rawValue(*old) == list.dump())
        return {};
    if (expressions.empty()) {
        auto removed = document->remove(kMember);
        if (!removed)
            return std::unexpected(removed.error());
        if (!*removed)
            return {};
    } else if (auto set = document->setRaw(kMember, list.dump()); !set)
        return set;
    if (auto ensured = settings::ensureProjectLayout(fs, *layout); !ensured)
        return ensured;
    return fs.writeFileAtomic(layout->personalSettings, document->bytes());
}
} // namespace frost::debug
