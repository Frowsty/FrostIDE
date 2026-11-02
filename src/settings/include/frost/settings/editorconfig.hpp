// SPDX-License-Identifier: Apache-2.0
//
// EditorConfig (feature row 59; PLAN.md section 3.6 "File-level" scope; ADR 0021): reading
// .editorconfig files, matching their section globs, and mapping the properties FrostIDE supports
// onto registry keys, layered per file over a SettingsSnapshot. Written from the public format
// description at editorconfig.org; no EditorConfig implementation was consulted.
/*
 * Files. For a file F, every directory from F's parent up to the file-system root is searched for
 * ".editorconfig"; the search stops after a file whose preamble says "root = true". Files closer to
 * F win; within a file, later sections win. Lines: "[glob]" opens a section, "key = value" sets a
 * property (key lower-cased; value trimmed, lower-cased for the properties below), lines starting
 * with '#' or ';' are comments. Inline comments are not stripped.
 *
 * Globs. '*' any run without '/', '**' any run, '?' one character but '/', "[abc]", "[a-z]",
 * "[!abc]", "{a,b,c}" (nestable), "{1..10}" (an integer in range), '\' escapes. A glob without '/'
 * matches the file name in any directory below the .editorconfig; one with '/' matches the path
 * relative to it (a leading '/' is dropped). Matching is case-sensitive.
 *
 * Mapping (value "unset" removes a property set by an outer file or earlier section):
 *   indent_style  space|tab          editor.insertSpaces
 *   indent_size   N|tab              editor.indentSize (tab: the tab width); also editor.tabSize when
 *                                    tab_width is not given
 *   tab_width     N                  editor.tabSize
 *   end_of_line   lf|crlf|cr         files.lineSeparator
 *   charset       latin1|utf-8|utf-8-bom|utf-16be|utf-16le
 *                                    files.encoding ("ISO-8859-1", "UTF-8", "UTF-16BE", "UTF-16LE")
 *                                    and files.utf8Bom (utf-8: false, utf-8-bom: true)
 *   trim_trailing_whitespace  true|false   editor.trimTrailingWhitespace ("all" / "none")
 *   insert_final_newline      true|false   editor.insertFinalNewline
 *   max_line_length           N|off        editor.rightMargin (off: not mapped)
 * Values outside a key's schema are skipped with a diagnostic. Only keys whose scopes include
 * FileLevel are mapped, and nothing is applied when editor.editorConfig is false.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"
#include "frost/settings/validation.hpp"
#include "frost/settings/value.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::settings {

// One parsed .editorconfig file.
struct EditorConfigFile {
    struct Section {
        std::string glob;
        std::vector<std::pair<std::string, std::string>> properties; // In file order.
    };
    bool root = false;
    std::vector<Section> sections;
};

// Parses .editorconfig text. Never fails: malformed lines are skipped. O(n).
[[nodiscard]] EditorConfigFile parseEditorConfig(std::string_view text);

// True when `glob` matches `relativePath` ('/'-separated, relative to the .editorconfig's directory),
// with the rules above (including the "no '/' matches the file name anywhere" rule). Worst case
// exponential in the number of '*' and braces; .editorconfig globs are short.
[[nodiscard]] bool editorConfigGlobMatches(std::string_view glob, std::string_view relativePath);

// The properties in force for one file, after "unset" removal. Sorted by name.
struct EditorConfigProperties {
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<std::string> files; // The .editorconfig files read, outermost first.
    [[nodiscard]] const std::string* find(std::string_view name) const noexcept;
};

/*
 * Resolves the properties for `filePath` (absolute) by reading .editorconfig files upwards (see the
 * file comment). Files that cannot be read (other than NotFound) are skipped and reported in
 * `diagnostics` when given. Errors: InvalidArgument for a relative path. O(depth * file size).
 */
[[nodiscard]] Result<EditorConfigProperties> resolveEditorConfig(io::IFileSystem& fs, std::string_view filePath,
                                                                 io::PathStyle style = io::kNativePathStyle,
                                                                 std::vector<Diagnostic>* diagnostics = nullptr);

// The registry values the properties map to (table above), sorted by key. Values that fail their
// schema are skipped and reported in `diagnostics` when given.
[[nodiscard]] std::vector<std::pair<std::string, Value>>
mapEditorConfig(const Registry& registry, const EditorConfigProperties& properties,
                std::vector<Diagnostic>* diagnostics = nullptr);

/*
 * The settings for one file: a snapshot plus the FileLevel values of its .editorconfig (empty when
 * editor.editorConfig is false in the snapshot). Immutable; any thread.
 */
class FileSettings {
public:
    FileSettings(std::shared_ptr<const SettingsSnapshot> snapshot, std::vector<std::pair<std::string, Value>> overlay);

    template<SettingType T>
    [[nodiscard]] const T& get(Key<T> key) const {
        return std::get<T>(value(key.name));
    }
    [[nodiscard]] const Value& value(std::string_view key) const;
    // FileLevel for a value from .editorconfig, otherwise the snapshot's source.
    [[nodiscard]] Scope source(std::string_view key) const;
    [[nodiscard]] const SettingsSnapshot& snapshot() const noexcept { return *m_snapshot; }

private:
    std::shared_ptr<const SettingsSnapshot> m_snapshot;
    std::vector<std::pair<std::string, Value>> m_overlay; // Sorted by key.
};

// resolveEditorConfig + mapEditorConfig over `snapshot`; read problems go to `diagnostics`. Errors:
// InvalidArgument for a relative path.
[[nodiscard]] Result<FileSettings> settingsForFile(std::shared_ptr<const SettingsSnapshot> snapshot,
                                                   io::IFileSystem& fs, std::string_view filePath,
                                                   io::PathStyle style = io::kNativePathStyle,
                                                   std::vector<Diagnostic>* diagnostics = nullptr);

} // namespace frost::settings
