// SPDX-License-Identifier: Apache-2.0
//
// SessionState's JSON codec and SessionStore (frost/ui/nav/session.hpp), on yyjson (MIT, ADR 0021).

#include "frost/ui/nav/session.hpp"

#include "frost/ui/nav/paths.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <utility>

#include <yyjson.h>

namespace frost::ui::nav {
namespace {

struct MutDocDeleter {
    void operator()(yyjson_mut_doc* doc) const noexcept { yyjson_mut_doc_free(doc); }
};
struct DocDeleter {
    void operator()(yyjson_doc* doc) const noexcept { yyjson_doc_free(doc); }
};
struct FreeDeleter {
    void operator()(char* text) const noexcept { std::free(text); }
};

void addString(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key, std::string_view value) {
    yyjson_mut_obj_add_strncpy(doc, object, key, value.data(), value.size());
}

[[nodiscard]] std::string writeJson(yyjson_mut_doc* doc) {
    std::size_t length = 0;
    const std::unique_ptr<char, FreeDeleter> text(yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &length));
    if (text == nullptr) {
        return "{}";
    }
    std::string out(text.get(), length);
    out.push_back('\n');
    return out;
}

[[nodiscard]] std::string stringOf(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string(yyjson_get_str(value), yyjson_get_len(value)) : std::string();
}

[[nodiscard]] int intOf(yyjson_val* value, int fallback) {
    if (!yyjson_is_int(value)) {
        return fallback;
    }
    const std::int64_t number = yyjson_get_sint(value);
    return static_cast<int>(std::clamp<std::int64_t>(number, -1, 1'000'000'000));
}

[[nodiscard]] bool boolOf(yyjson_val* value) { return yyjson_is_bool(value) && yyjson_get_bool(value); }

void addPlace(yyjson_mut_doc* doc, yyjson_mut_val* array, const Place& place) {
    yyjson_mut_val* item = yyjson_mut_obj(doc);
    addString(doc, item, "path", place.path);
    yyjson_mut_obj_add_int(doc, item, "line", place.line);
    yyjson_mut_obj_add_int(doc, item, "column", place.column);
    yyjson_mut_arr_append(array, item);
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

} // namespace

std::string encodeSession(const SessionState& state) {
    const std::unique_ptr<yyjson_mut_doc, MutDocDeleter> doc(yyjson_mut_doc_new(nullptr));
    yyjson_mut_val* root = yyjson_mut_obj(doc.get());
    yyjson_mut_doc_set_root(doc.get(), root);
    yyjson_mut_obj_add_int(doc.get(), root, "version", kSessionVersion);
    addString(doc.get(), root, "root", state.root);
    yyjson_mut_val* tabs = yyjson_mut_arr(doc.get());
    for (const SessionTab& tab : state.tabs) {
        yyjson_mut_val* item = yyjson_mut_obj(doc.get());
        addString(doc.get(), item, "path", tab.path);
        yyjson_mut_obj_add_bool(doc.get(), item, "pinned", tab.pinned);
        yyjson_mut_obj_add_int(doc.get(), item, "line", tab.line);
        yyjson_mut_obj_add_int(doc.get(), item, "column", tab.column);
        yyjson_mut_obj_add_int(doc.get(), item, "top", tab.top);
        yyjson_mut_obj_add_int(doc.get(), item, "group", tab.group);
        yyjson_mut_obj_add_bool(doc.get(), item, "groupActive", tab.groupActive);
        if (!tab.folds.empty()) {
            yyjson_mut_obj_add_uint(doc.get(), item, "foldTextHash", tab.foldTextHash);
            yyjson_mut_val* folds = yyjson_mut_arr(doc.get());
            for (const auto& [begin, end] : tab.folds) {
                yyjson_mut_val* pair = yyjson_mut_arr(doc.get());
                yyjson_mut_arr_add_uint(doc.get(), pair, begin);
                yyjson_mut_arr_add_uint(doc.get(), pair, end);
                yyjson_mut_arr_append(folds, pair);
            }
            yyjson_mut_obj_add_val(doc.get(), item, "folds", folds);
        }
        yyjson_mut_arr_append(tabs, item);
    }
    yyjson_mut_obj_add_val(doc.get(), root, "tabs", tabs);
    yyjson_mut_obj_add_int(doc.get(), root, "active", state.active);
    if (!state.editorLayout.empty())
        addString(doc.get(), root, "editorLayout", state.editorLayout);
    yyjson_mut_obj_add_int(doc.get(), root, "activeGroup", state.activeGroup);
    yyjson_mut_val* windows = yyjson_mut_arr(doc.get());
    for (const SessionToolWindow& window : state.toolWindows) {
        yyjson_mut_val* item = yyjson_mut_obj(doc.get());
        addString(doc.get(), item, "id", window.id);
        yyjson_mut_obj_add_bool(doc.get(), item, "visible", window.visible);
        yyjson_mut_obj_add_int(doc.get(), item, "size", window.size);
        yyjson_mut_arr_append(windows, item);
    }
    yyjson_mut_obj_add_val(doc.get(), root, "toolWindows", windows);
    yyjson_mut_val* files = yyjson_mut_arr(doc.get());
    for (const std::string& path : state.recentFiles) {
        yyjson_mut_arr_add_strncpy(doc.get(), files, path.data(), path.size());
    }
    yyjson_mut_obj_add_val(doc.get(), root, "recentFiles", files);
    yyjson_mut_val* locations = yyjson_mut_arr(doc.get());
    for (const Place& place : state.recentLocations) {
        addPlace(doc.get(), locations, place);
    }
    yyjson_mut_obj_add_val(doc.get(), root, "recentLocations", locations);
    return writeJson(doc.get());
}

Result<SessionState> decodeSession(std::string_view json) {
    yyjson_read_err error{};
    const std::unique_ptr<yyjson_doc, DocDeleter> doc(
        yyjson_read_opts(const_cast<char*>(json.data()), json.size(), YYJSON_READ_NOFLAG, nullptr, &error));
    if (doc == nullptr) {
        return fail(ErrorCode::Corrupt, std::format("session: not JSON at byte {}: {}", error.pos,
                                                    error.msg != nullptr ? error.msg : "unknown error"));
    }
    yyjson_val* root = yyjson_doc_get_root(doc.get());
    if (!yyjson_is_obj(root)) {
        return fail(ErrorCode::Corrupt, "session: the document is not an object");
    }
    const int version = intOf(yyjson_obj_get(root, "version"), 0);
    if (version < 1) {
        return fail(ErrorCode::Corrupt, "session: no version");
    }
    if (version > kSessionVersion) {
        return fail(ErrorCode::Unsupported, std::format("session: version {} is newer than this build", version));
    }
    SessionState state;
    state.root = stringOf(yyjson_obj_get(root, "root"));
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val* item = nullptr;
    if (yyjson_val* tabs = yyjson_obj_get(root, "tabs"); yyjson_is_arr(tabs)) {
        yyjson_arr_foreach(tabs, index, count, item) {
            SessionTab tab;
            tab.path = stringOf(yyjson_obj_get(item, "path"));
            tab.pinned = boolOf(yyjson_obj_get(item, "pinned"));
            tab.line = std::max(1, intOf(yyjson_obj_get(item, "line"), 1));
            tab.column = std::max(1, intOf(yyjson_obj_get(item, "column"), 1));
            tab.top = std::max(0, intOf(yyjson_obj_get(item, "top"), 0));
            tab.group = std::clamp(intOf(yyjson_obj_get(item, "group"), 0), 0, 63);
            tab.groupActive = boolOf(yyjson_obj_get(item, "groupActive"));
            yyjson_val* hash = yyjson_obj_get(item, "foldTextHash");
            yyjson_val* folds = yyjson_obj_get(item, "folds");
            if (yyjson_is_uint(hash) && yyjson_is_arr(folds) && yyjson_arr_size(folds) <= 100'000) {
                tab.foldTextHash = yyjson_get_uint(hash);
                std::size_t foldIndex = 0, foldCount = 0;
                yyjson_val* pair = nullptr;
                yyjson_arr_foreach(folds, foldIndex, foldCount, pair) {
                    yyjson_val* begin = yyjson_arr_get(pair, 0);
                    yyjson_val* end = yyjson_arr_get(pair, 1);
                    if (!yyjson_is_arr(pair) || yyjson_arr_size(pair) != 2 || !yyjson_is_uint(begin) ||
                        !yyjson_is_uint(end) || yyjson_get_uint(begin) >= yyjson_get_uint(end)) {
                        tab.folds.clear();
                        tab.foldTextHash = 0;
                        break;
                    }
                    tab.folds.emplace_back(yyjson_get_uint(begin), yyjson_get_uint(end));
                }
            }
            if (!tab.path.empty()) {
                state.tabs.push_back(std::move(tab));
            }
        }
    }
    state.editorLayout = stringOf(yyjson_obj_get(root, "editorLayout"));
    state.activeGroup = std::clamp(intOf(yyjson_obj_get(root, "activeGroup"), 0), 0, 63);
    state.active = intOf(yyjson_obj_get(root, "active"), -1);
    if (state.active >= static_cast<int>(state.tabs.size())) {
        state.active = state.tabs.empty() ? -1 : 0;
    }
    if (yyjson_val* windows = yyjson_obj_get(root, "toolWindows"); yyjson_is_arr(windows)) {
        yyjson_arr_foreach(windows, index, count, item) {
            SessionToolWindow window;
            window.id = stringOf(yyjson_obj_get(item, "id"));
            window.visible = boolOf(yyjson_obj_get(item, "visible"));
            window.size = std::max(0, intOf(yyjson_obj_get(item, "size"), 0));
            if (!window.id.empty()) {
                state.toolWindows.push_back(std::move(window));
            }
        }
    }
    if (yyjson_val* files = yyjson_obj_get(root, "recentFiles"); yyjson_is_arr(files)) {
        yyjson_arr_foreach(files, index, count, item) {
            if (std::string path = stringOf(item); !path.empty()) {
                state.recentFiles.push_back(std::move(path));
            }
        }
    }
    if (yyjson_val* locations = yyjson_obj_get(root, "recentLocations"); yyjson_is_arr(locations)) {
        yyjson_arr_foreach(locations, index, count, item) {
            Place place{stringOf(yyjson_obj_get(item, "path")), std::max(1, intOf(yyjson_obj_get(item, "line"), 1)),
                        std::max(1, intOf(yyjson_obj_get(item, "column"), 1))};
            if (!place.path.empty()) {
                state.recentLocations.push_back(std::move(place));
            }
        }
    }
    return state;
}

std::string sessionKey(std::string_view root) { return std::format("{:016x}", fnv1a(root)); }

SessionStore::SessionStore(io::IFileSystem& fs, std::string directory) : m_fs(&fs), m_directory(std::move(directory)) {}

std::string SessionStore::fileOf(std::string_view root) const {
    return joinIn(m_directory, std::format("project-{}.json", sessionKey(root)));
}

Result<SessionState> SessionStore::load(std::string_view root) const {
    auto bytes = m_fs->readFile(fileOf(root));
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto state = decodeSession(*bytes);
    if (state && state->root.empty()) {
        state->root = std::string(root);
    }
    return state;
}

Result<void> SessionStore::save(const SessionState& state) {
    if (auto created = m_fs->createDirectories(m_directory); !created) {
        return created;
    }
    if (auto written = m_fs->writeFileAtomic(fileOf(state.root), encodeSession(state)); !written) {
        return written;
    }
    return setLastProject(state.root);
}

std::vector<std::string> SessionStore::recentProjects() const {
    std::vector<std::string> recent;
    auto bytes = m_fs->readFile(joinIn(m_directory, "index.json"));
    if (!bytes) {
        return recent;
    }
    const std::unique_ptr<yyjson_doc, DocDeleter> doc(
        yyjson_read_opts(bytes->data(), bytes->size(), YYJSON_READ_NOFLAG, nullptr, nullptr));
    if (doc == nullptr) {
        return recent;
    }
    yyjson_val* root = yyjson_doc_get_root(doc.get());
    if (std::string last = stringOf(yyjson_obj_get(root, "lastProject")); !last.empty()) {
        recent.push_back(std::move(last));
    }
    if (yyjson_val* projects = yyjson_obj_get(root, "recentProjects"); yyjson_is_arr(projects)) {
        std::size_t index = 0;
        std::size_t count = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(projects, index, count, item) {
            std::string path = stringOf(item);
            if (!path.empty() && std::ranges::find(recent, path) == recent.end()) {
                recent.push_back(std::move(path));
            }
        }
    }
    return recent;
}

std::optional<std::string> SessionStore::lastProject() const {
    auto bytes = m_fs->readFile(joinIn(m_directory, "index.json"));
    if (!bytes) {
        return std::nullopt;
    }
    const std::unique_ptr<yyjson_doc, DocDeleter> doc(
        yyjson_read_opts(bytes->data(), bytes->size(), YYJSON_READ_NOFLAG, nullptr, nullptr));
    if (doc == nullptr) {
        return std::nullopt;
    }
    std::string last = stringOf(yyjson_obj_get(yyjson_doc_get_root(doc.get()), "lastProject"));
    if (last.empty()) {
        return std::nullopt;
    }
    return last;
}

Result<void> SessionStore::setLastProject(std::string_view root) {
    std::vector<std::string> recent = recentProjects();
    std::erase_if(recent, [root](const std::string& entry) { return entry == root; });
    recent.insert(recent.begin(), std::string(root));
    if (recent.size() > 20) {
        recent.resize(20);
    }
    return writeIndex(recent, recent.front());
}

Result<void> SessionStore::clearLastProject() {
    if (!lastProject()) {
        return {};
    }
    return writeIndex(recentProjects(), {});
}

Result<void> SessionStore::writeIndex(const std::vector<std::string>& recent, std::string_view last) {
    if (auto created = m_fs->createDirectories(m_directory); !created) {
        return created;
    }
    const std::unique_ptr<yyjson_mut_doc, MutDocDeleter> doc(yyjson_mut_doc_new(nullptr));
    yyjson_mut_val* root = yyjson_mut_obj(doc.get());
    yyjson_mut_doc_set_root(doc.get(), root);
    yyjson_mut_obj_add_int(doc.get(), root, "version", kSessionVersion);
    addString(doc.get(), root, "lastProject", last);
    yyjson_mut_val* projects = yyjson_mut_arr(doc.get());
    for (const std::string& path : recent) {
        yyjson_mut_arr_add_strncpy(doc.get(), projects, path.data(), path.size());
    }
    yyjson_mut_obj_add_val(doc.get(), root, "recentProjects", projects);
    return m_fs->writeFileAtomic(joinIn(m_directory, "index.json"), writeJson(doc.get()));
}

} // namespace frost::ui::nav
