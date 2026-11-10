// SPDX-License-Identifier: Apache-2.0
//
// A project's .luarc.json for the Lua settings page (lua_rc.hpp, LUA-4): reading the settings in
// LuaLS's three key forms, applying edits while keeping every other key, and the checked write.

#include "frost/lang/lua_rc.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"

#include <array>
#include <format>
#include <optional>

namespace frost::lang {
namespace {

// A setting as its two-level path ("runtime", "version").
struct Setting {
    std::string_view group;
    std::string_view name;
    [[nodiscard]] std::string dotted() const { return std::format("{}.{}", group, name); }
    [[nodiscard]] std::string prefixed() const { return std::format("Lua.{}.{}", group, name); }
};
constexpr Setting kVersion{"runtime", "version"};
constexpr Setting kLibrary{"workspace", "library"};
constexpr Setting kGlobals{"diagnostics", "globals"};
constexpr Setting kDisable{"diagnostics", "disable"};
constexpr Setting kEnable{"diagnostics", "enable"};

// `object` without the member `key` (members keep their order).
[[nodiscard]] Json without(const Json& object, std::string_view key) {
    Json out = Json::object();
    const auto& keys = object.keys();
    const auto& values = object.items();
    for (std::size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        if (keys[i] != key) {
            out.set(keys[i], values[i]);
        }
    }
    return out;
}

// The value of `setting` in the first key form present, or null.
[[nodiscard]] const Json* find(const Json& root, const Setting& setting) {
    if (const Json* value = root.find(setting.prefixed())) {
        return value;
    }
    if (const Json* value = root.find(setting.dotted())) {
        return value;
    }
    if (const Json* group = root.find(setting.group); group != nullptr && group->isObject()) {
        return group->find(setting.name);
    }
    return nullptr;
}

// Sets (`value` non-null) or removes `setting` in every form present; adds the nested form when
// none is present and a value is given.
void update(Json& root, const Setting& setting, std::optional<Json> value) {
    bool present = false;
    for (const std::string& key : {setting.prefixed(), setting.dotted()}) {
        if (root.find(key) == nullptr) {
            continue;
        }
        present = true;
        if (value) {
            root.set(key, *value);
        } else {
            root = without(root, key);
        }
    }
    Json* group = root.find(setting.group);
    if (group != nullptr && group->isObject() && group->find(setting.name) != nullptr) {
        present = true;
        if (value) {
            group->set(std::string(setting.name), *value);
        } else {
            *group = without(*group, setting.name);
            if (group->keys().empty()) {
                root = without(root, setting.group);
            }
        }
    }
    if (!present && value) {
        if (group == nullptr || !group->isObject()) {
            root.set(std::string(setting.group), Json::object());
            group = root.find(setting.group);
        }
        group->set(std::string(setting.name), std::move(*value));
    }
}

[[nodiscard]] Result<std::vector<std::string>> stringList(const Json* value, const Setting& setting) {
    std::vector<std::string> out;
    if (value == nullptr || value->isNull()) {
        return out;
    }
    if (!value->isArray()) {
        return fail(ErrorCode::Corrupt, std::format("{} is not a list", setting.dotted()));
    }
    for (const Json& item : value->items()) {
        const std::string* text = item.asString();
        if (text == nullptr) {
            return fail(ErrorCode::Corrupt, std::format("{} holds a value that is not a string", setting.dotted()));
        }
        out.push_back(*text);
    }
    return out;
}

[[nodiscard]] std::optional<Json> listValue(const std::vector<std::string>& items) {
    if (items.empty()) {
        return std::nullopt;
    }
    Json list = Json::array();
    for (const std::string& item : items) {
        list.push(item);
    }
    return list;
}

[[nodiscard]] Result<Json> parseRoot(std::string_view text) {
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return Json::object();
    }
    auto parsed = parseJson(text);
    if (!parsed) {
        return fail(ErrorCode::Corrupt,
                    std::format("{} is not valid JSON: {}", kLuaRcFileName, parsed.error().message()));
    }
    if (!parsed->isObject()) {
        return fail(ErrorCode::Corrupt, std::format("{} does not hold a JSON object", kLuaRcFileName));
    }
    return std::move(*parsed);
}

void pretty(const Json& value, std::string& out, int depth) {
    const auto indent = [&out](int level) { out.append(static_cast<std::size_t>(level) * 4, ' '); };
    if (value.isArray() && value.size() > 0) {
        out += "[\n";
        for (std::size_t i = 0; i < value.items().size(); ++i) {
            indent(depth + 1);
            pretty(value.items()[i], out, depth + 1);
            out += i + 1 < value.items().size() ? ",\n" : "\n";
        }
        indent(depth);
        out += ']';
    } else if (value.isObject() && !value.keys().empty()) {
        out += "{\n";
        const auto& keys = value.keys();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            indent(depth + 1);
            out += Json(keys[i]).dump();
            out += ": ";
            pretty(value.items()[i], out, depth + 1);
            out += i + 1 < keys.size() ? ",\n" : "\n";
        }
        indent(depth);
        out += '}';
    } else {
        out += value.dump();
    }
}

} // namespace

Result<LuaRcSettings> parseLuaRc(std::string_view text) {
    auto root = parseRoot(text);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    LuaRcSettings settings;
    if (const Json* version = find(*root, kVersion); version != nullptr && !version->isNull()) {
        const std::string* value = version->asString();
        if (value == nullptr) {
            return fail(ErrorCode::Corrupt, "runtime.version is not a string");
        }
        settings.runtimeVersion = *value;
    }
    auto library = stringList(find(*root, kLibrary), kLibrary);
    auto globals = stringList(find(*root, kGlobals), kGlobals);
    auto disable = stringList(find(*root, kDisable), kDisable);
    if (!library || !globals || !disable) {
        return std::unexpected(!library ? library.error() : !globals ? globals.error() : disable.error());
    }
    settings.libraries = std::move(*library);
    settings.globals = std::move(*globals);
    settings.disabledDiagnostics = std::move(*disable);
    if (const Json* enable = find(*root, kEnable); enable != nullptr && !enable->isNull()) {
        const auto flag = enable->asBool();
        if (!flag) {
            return fail(ErrorCode::Corrupt, "diagnostics.enable is not a boolean");
        }
        settings.diagnosticsEnabled = *flag;
    }
    return settings;
}

Result<std::string> applyLuaRc(std::string_view text, const LuaRcSettings& settings) {
    auto root = parseRoot(text);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    update(*root, kVersion,
           settings.runtimeVersion.empty() ? std::nullopt : std::optional<Json>(Json(settings.runtimeVersion)));
    update(*root, kLibrary, listValue(settings.libraries));
    update(*root, kGlobals, listValue(settings.globals));
    update(*root, kDisable, listValue(settings.disabledDiagnostics));
    update(*root, kEnable, settings.diagnosticsEnabled ? std::nullopt : std::optional<Json>(Json(false)));
    std::string out;
    pretty(*root, out, 0);
    out += '\n';
    return out;
}

Result<LuaRcFile> readLuaRc(io::IFileSystem& fs, std::string_view root) {
    if (!io::isAbsolutePath(root)) {
        return fail(ErrorCode::InvalidArgument, "the Lua project folder must be absolute");
    }
    auto path = io::joinPath(root, kLuaRcFileName);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    LuaRcFile file;
    file.path = std::move(*path);
    if (auto text = fs.readFile(file.path)) {
        file.exists = true;
        file.text = std::move(*text);
    } else if (text.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(text.error()));
    }
    auto settings = parseLuaRc(file.text);
    if (!settings) {
        return std::unexpected(std::move(settings.error().withContext(file.path)));
    }
    file.settings = std::move(*settings);
    return file;
}

Result<void> writeLuaRc(io::IFileSystem& fs, const LuaRcFile& loaded, const LuaRcSettings& settings) {
    auto current = fs.readFile(loaded.path);
    if (!current && current.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(current.error()));
    }
    const bool existsNow = current.has_value();
    if (existsNow != loaded.exists || (existsNow && *current != loaded.text)) {
        return fail(ErrorCode::Busy, std::format("{} changed on disk since it was read; reload it", loaded.path));
    }
    auto text = applyLuaRc(loaded.text, settings);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    if (loaded.exists && *text == loaded.text) {
        return {};
    }
    if (!loaded.exists && settings == LuaRcSettings{}) {
        return {}; // Nothing to say: no file is created for the defaults.
    }
    return fs.writeFileAtomic(loaded.path, *text);
}

} // namespace frost::lang
