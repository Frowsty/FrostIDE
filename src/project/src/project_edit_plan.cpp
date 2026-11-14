// SPDX-License-Identifier: Apache-2.0
#include "frost/project/project_edit.hpp"

#include "frost/io/path.hpp"
#include "json.hpp"
#include "paths.hpp"

#include <algorithm>
#include <set>

namespace frost::project {
namespace {

Result<std::string> decode(std::string_view value) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (value.size() % 4 || value.size() > 12 * 1024 * 1024)
        return fail(ErrorCode::Corrupt, "Invalid or oversized ProjectHost base64 input");
    std::string result;
    result.reserve(value.size() / 4 * 3);
    for (std::size_t offset = 0; offset < value.size(); offset += 4) {
        unsigned digits[4]{};
        unsigned padding = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char byte = value[offset + i];
            if (byte == '=') {
                if (i < 2 || offset + 4 != value.size())
                    return fail(ErrorCode::Corrupt, "Invalid ProjectHost base64 padding");
                ++padding;
            } else {
                const auto index = alphabet.find(byte);
                if (padding || index == std::string_view::npos)
                    return fail(ErrorCode::Corrupt, "Invalid ProjectHost base64 byte");
                digits[i] = static_cast<unsigned>(index);
            }
        }
        if (padding > 2 || (padding == 2 && (digits[1] & 15)) || (padding == 1 && (digits[2] & 3)))
            return fail(ErrorCode::Corrupt, "Noncanonical ProjectHost base64 padding");
        result += static_cast<char>((digits[0] << 2) | (digits[1] >> 4));
        if (padding < 2)
            result += static_cast<char>((digits[1] << 4) | (digits[2] >> 2));
        if (!padding)
            result += static_cast<char>((digits[2] << 6) | digits[3]);
    }
    return result;
}

bool uniqueKeys(const detail::JsonNode& node) {
    if (node.isObject()) {
        std::set<std::string> keys;
        for (const auto& key : node.keys)
            if (!keys.insert(key).second)
                return false;
    }
    return std::ranges::all_of(node.items, [](const auto& child) { return uniqueKeys(child); });
}

Result<std::string> bytes(const detail::JsonNode& object, std::string_view name) {
    const auto* value = object.find(name);
    if (!value || !value->isString())
        return fail(ErrorCode::Corrupt, "ProjectHost plan omitted required input bytes");
    return decode(value->text);
}

} // namespace

Result<ProjectEditPlan> parseProjectEditPlan(std::string_view line) {
    if (line.size() > 64 * 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "ProjectHost plan exceeds 64 MiB");
    auto root = detail::parseJson(line);
    if (!root || !root->isObject() || !uniqueKeys(*root))
        return fail(ErrorCode::Corrupt, "Malformed or duplicate-key ProjectHost plan");
    const auto* error = root->find("Error");
    const auto* edit = root->find("Edit");
    if (!error || !error->isNull() || !edit || !edit->isObject() ||
        edit->integerAt("PlanVersion") != 1 || edit->stringAt("Mode") != "BeforeFileMoveRename")
        return fail(ErrorCode::Corrupt, "Expected a successful versioned pre-move ProjectHost plan");
    ProjectEditPlan result;
    result.projectPath = edit->stringAt("ProjectPath");
    result.environment = edit->stringAt("Environment");
    if (!io::isAbsolutePath(result.projectPath) || result.environment.empty())
        return fail(ErrorCode::Corrupt, "ProjectHost plan omitted project or environment identity");
    auto before = bytes(*edit, "ProjectBefore");
    auto after = bytes(*edit, "Project");
    if (!before || !after || before->empty() || after->empty())
        return fail(ErrorCode::Corrupt, "ProjectHost project bytes are invalid");
    result.projectBefore = std::move(*before);
    result.projectAfter = std::move(*after);
    const auto* filterBefore = edit->find("FiltersBefore");
    const auto* filterAfter = edit->find("Filters");
    if (!filterBefore || !filterAfter || filterBefore->isNull() != filterAfter->isNull())
        return fail(ErrorCode::Corrupt, "ProjectHost filters presence changed during rename planning");
    if (!filterBefore->isNull()) {
        auto old = bytes(*edit, "FiltersBefore");
        auto next = bytes(*edit, "Filters");
        if (!old || !next)
            return fail(ErrorCode::Corrupt, "ProjectHost filter bytes are invalid");
        result.filtersBefore = std::move(*old);
        result.filtersAfter = std::move(*next);
    }
    const auto* configurations = edit->find("Configurations");
    const auto* imports = edit->find("InputBytes");
    if (!configurations || !configurations->isArray() || configurations->items.empty() ||
        configurations->items.size() > 256 || !imports || !imports->isArray() || imports->items.size() > 512)
        return fail(ErrorCode::Corrupt, "ProjectHost plan omitted bounded configuration/import evidence");
    std::set<std::string> seen;
    for (const auto& configuration : configurations->items) {
        if (!configuration.isString() || configuration.text.empty() || !seen.insert(configuration.text).second)
            return fail(ErrorCode::Corrupt, "ProjectHost configurations are malformed or duplicated");
        result.configurations.push_back(configuration.text);
    }
    seen.clear();
    std::size_t total = result.projectBefore.size() + result.projectAfter.size();
    for (const auto& input : imports->items) {
        const auto path = input.stringAt("Path");
        auto content = bytes(input, "Before");
        if (!input.isObject() || !io::isAbsolutePath(path) || !content ||
            !seen.insert(detail::pathKey(path)).second)
            return fail(ErrorCode::Corrupt, "ProjectHost import inputs are malformed or duplicated");
        total += content->size();
        if (total > 48 * 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "ProjectHost decoded plan exceeds 48 MiB");
        result.imports.push_back({path, std::move(*content)});
    }
    return result;
}

} // namespace frost::project
