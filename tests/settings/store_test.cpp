// SPDX-License-Identifier: Apache-2.0
//
// Tests for SettingsStore (store.hpp) on MemFs: defaults, scope precedence, invalid values and
// disallowed scopes, byte-exact writes that keep unknown keys, the .frost/ layout and its .gitignore,
// the trust rule for `executes` keys, live reload through MemFsWatcher under a SimExecutor, validation
// on save, the stand-in actions, project switching and the application settings path.

#include "frost/io/mem_fs.hpp"
#include "frost/io/mem_fs_watcher.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/settings/store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace frost::settings;
using frost::ErrorCode;
using frost::io::MemFs;
using frost::io::PathStyle;

constexpr std::string_view kAppFile = "/home/u/.config/frostide/settings.json";
constexpr std::string_view kRoot = "/work/app";
constexpr std::string_view kShared = "/work/app/.frost/settings.json";
constexpr std::string_view kPersonal = "/work/app/.frost/workspace.json";

struct StoreTest : ::testing::Test {
    frost::ManualClock clock;
    MemFs fs{clock, PathStyle::Posix};

    [[nodiscard]] SettingsPaths paths(bool withProject = true) const {
        SettingsPaths result{.applicationFile = std::string(kAppFile), .project = std::nullopt};
        if (withProject) {
            result.project = ProjectLayout::forRoot(kRoot, PathStyle::Posix).value();
        }
        return result;
    }
    [[nodiscard]] std::unique_ptr<SettingsStore> open(bool trusted = false, bool withProject = true) {
        auto store = SettingsStore::open(
            builtinRegistry(), fs, paths(withProject),
            StoreOptions{.projectTrusted = trusted, .migrations = nullptr, .style = PathStyle::Posix});
        EXPECT_TRUE(store.has_value()) << (store ? "" : store.error().describe());
        return store ? std::move(*store) : nullptr;
    }
    void write(std::string_view path, std::string_view text) {
        ASSERT_TRUE(fs.createDirectories(frost::io::parentPath(path, PathStyle::Posix)));
        ASSERT_TRUE(fs.writeFileAtomic(path, text));
    }
    [[nodiscard]] std::string read(std::string_view path) { return fs.readFile(path).value_or("<missing>"); }
    void SetUp() override { ASSERT_TRUE(fs.createDirectories(kRoot)); }
};

TEST_F(StoreTest, DefaultsWithoutFiles) {
    const auto store = open();
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot->get(key::editorFontFamily), "JetBrains Mono");
    EXPECT_EQ(snapshot->get(key::editorFontSize), 13);
    EXPECT_EQ(snapshot->get(key::editorTabSize), 4);
    EXPECT_TRUE(snapshot->get(key::editorInsertSpaces));
    EXPECT_FALSE(snapshot->get(key::editorSoftWrap));
    EXPECT_TRUE(snapshot->get(key::editorLineNumbers));
    EXPECT_EQ(snapshot->get(key::uiTheme), "glacier-dark");
    EXPECT_EQ(snapshot->get(key::keymapId), "intellij");
    EXPECT_EQ(snapshot->source(key::editorTabSize.name), Scope::Default);
    EXPECT_TRUE(snapshot->diagnostics().empty());
    // Opening a project never creates anything in it.
    EXPECT_FALSE(fs.exists("/work/app/.frost").value());
}

TEST_F(StoreTest, ScopePrecedence) {
    write(kAppFile, R"({"schemaVersion": 1, "editor.tabSize": 2, "editor.softWrap": true, "editor.fontSize": 15})");
    write(kShared, R"({"schemaVersion": 1, "editor.tabSize": 3, "editor.softWrap": false})");
    write(kPersonal, R"({"schemaVersion": 1, "editor.tabSize": 8, "layout": {"left": 200}})");
    const auto store = open();
    auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot->get(key::editorTabSize), 8); // Personal beats shared beats application.
    EXPECT_EQ(snapshot->source(key::editorTabSize.name), Scope::ProjectPersonal);
    EXPECT_FALSE(snapshot->get(key::editorSoftWrap));
    EXPECT_EQ(snapshot->source(key::editorSoftWrap.name), Scope::ProjectShared);
    EXPECT_EQ(snapshot->get(key::editorFontSize), 15);
    EXPECT_EQ(snapshot->source(key::editorFontSize.name), Scope::Application);
    EXPECT_TRUE(snapshot->diagnostics().empty()); // "layout" is another feature's member.

    ASSERT_TRUE(store->reset(Scope::ProjectPersonal, key::editorTabSize.name));
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 3);
    ASSERT_TRUE(store->reset(Scope::ProjectShared, key::editorTabSize.name));
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 2);
    ASSERT_TRUE(store->reset(Scope::Application, key::editorTabSize.name));
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 4);
    EXPECT_NE(read(kPersonal).find("\"layout\": {\"left\": 200}"), std::string::npos);
}

TEST_F(StoreTest, InvalidValuesAndDisallowedScopesFallBack) {
    write(kAppFile, R"({"editor.tabSize": 2, "editor.fontSzie": 20})");
    write(kShared, "{\n  \"editor.tabSize\": 99,\n  \"ui.theme\": \"glacier-light\"\n}");
    const auto store = open();
    const auto snapshot = store->snapshot();
    EXPECT_EQ(snapshot->get(key::editorTabSize), 2);        // 99 is out of range: the application value applies.
    EXPECT_EQ(snapshot->get(key::uiTheme), "glacier-dark"); // The theme is per user.
    std::map<std::string, Severity> found;
    for (const Diagnostic& diagnostic : snapshot->diagnostics()) {
        found[diagnostic.key] = diagnostic.severity;
    }
    EXPECT_EQ(found["editor.tabSize"], Severity::Error);
    EXPECT_EQ(found["ui.theme"], Severity::Error);
    EXPECT_EQ(found["editor.fontSzie"], Severity::Warning);
    const auto tab = std::ranges::find(snapshot->diagnostics(), std::string("editor.tabSize"), &Diagnostic::key);
    ASSERT_NE(tab, snapshot->diagnostics().end());
    EXPECT_EQ(tab->line, 2U);
    EXPECT_EQ(tab->column, 3U);
    EXPECT_EQ(tab->path, kShared);
}

TEST_F(StoreTest, SetWritesByteExactlyAndKeepsUnknownKeys) {
    const std::string original = "{\n"
                                 "  // mine\n"
                                 "  \"schemaVersion\": 1,\n"
                                 "  \"editor.fontSize\": 14,\n"
                                 "  \"someone.else\": [\"untouched\",   1]\n"
                                 "}\n";
    write(kAppFile, original);
    const auto store = open();
    std::vector<std::vector<std::string>> heard;
    const Subscription subscription =
        store->subscribe([&](const SettingsChange& change) { heard.push_back(change.keys); });

    ASSERT_TRUE(store->set(Scope::Application, key::editorFontSize, std::int64_t{16}));
    std::string expected = original;
    expected.replace(expected.find("14"), 2, "16");
    EXPECT_EQ(read(kAppFile), expected);
    EXPECT_EQ(store->snapshot()->get(key::editorFontSize), 16);
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(heard[0], std::vector<std::string>{"editor.fontSize"});

    ASSERT_TRUE(store->set(Scope::Application, key::editorSoftWrap, true));
    EXPECT_EQ(read(kAppFile), expected.replace(expected.find("1]\n"), 3, "1],\n  \"editor.softWrap\": true\n"));

    // Refused: unknown key, wrong scope, bad value, Default scope. The file stays as it was.
    EXPECT_EQ(store->set(Scope::Application, "editor.nothing", Value(true)).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(store->set(Scope::ProjectShared, key::uiTheme, std::string("glacier-light")).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(store->set(Scope::Application, key::editorTabSize, std::int64_t{0}).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(store->set(Scope::Default, key::editorTabSize, std::int64_t{2}).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(read(kAppFile), expected);
}

TEST_F(StoreTest, SetRefusesABrokenFileAndAFileFromTheFuture) {
    const std::string broken = "{\"editor.tabSize\": 2,,}";
    write(kAppFile, broken);
    const auto store = open();
    const auto result = store->set(Scope::Application, key::editorTabSize, std::int64_t{3});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(read(kAppFile), broken);

    const std::string future = "{\"schemaVersion\": 99, \"editor.tabSize\": 2}";
    write(kShared, future);
    ASSERT_EQ(store->reload(Scope::ProjectShared).size(), 1U);
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 2); // Still read as far as understood.
    EXPECT_EQ(store->set(Scope::ProjectShared, key::editorTabSize, std::int64_t{3}).error().code(),
              ErrorCode::Unsupported);
    EXPECT_EQ(read(kShared), future);
}

TEST_F(StoreTest, ProjectWritesCreateTheFrostLayout) {
    const auto store = open();
    ASSERT_TRUE(store->set(Scope::ProjectPersonal, key::editorSoftWrap, true));
    EXPECT_EQ(read("/work/app/.frost/.gitignore"), kFrostGitignoreText);
    EXPECT_EQ(read(kPersonal), "{\n  \"schemaVersion\": 1,\n  \"editor.softWrap\": true\n}\n");
    EXPECT_EQ(store->snapshot()->source(key::editorSoftWrap.name), Scope::ProjectPersonal);

    // A user-edited .gitignore keeps its content and gains the entry once.
    write("/work/app/.frost/.gitignore", "*.log\r\n");
    const auto layout = ProjectLayout::forRoot(kRoot, PathStyle::Posix).value();
    ASSERT_TRUE(ensureProjectLayout(fs, layout));
    ASSERT_TRUE(ensureProjectLayout(fs, layout));
    EXPECT_EQ(read("/work/app/.frost/.gitignore"), "*.log\r\nworkspace.json\r\n");
    EXPECT_EQ(layout.runConfigurations, "/work/app/.frost/runConfigurations");
    EXPECT_EQ(layout.inspectionProfiles, "/work/app/.frost/inspectionProfiles");

    // Resetting in a scope without a file creates nothing.
    ASSERT_FALSE(fs.exists(kShared).value());
    ASSERT_TRUE(store->reset(Scope::ProjectShared, key::editorSoftWrap.name));
    EXPECT_FALSE(fs.exists(kShared).value());
}

TEST_F(StoreTest, ExecutesKeysOfUntrustedProjectsAreIgnored) {
    // The registry test PLAN.md 3.6 asks for: every executes key that a project scope may set is
    // ignored there until the project is trusted, and the trust banner lists it.
    const Registry& registry = builtinRegistry();
    std::string shared = "{\n";
    std::vector<const KeySpec*> gated;
    for (const KeySpec& spec : registry.keys()) {
        if (spec.executes && (spec.scopes.has(Scope::ProjectShared) || spec.scopes.has(Scope::ProjectPersonal))) {
            gated.push_back(&spec);
            ASSERT_TRUE(std::holds_alternative<StringList>(spec.defaultValue)) << spec.key << ": extend this test";
            shared += std::format("  \"{}\": [\"/planted/tool\"],\n", spec.key);
        }
    }
    ASSERT_FALSE(gated.empty());
    // Tool paths are Application-only, so a project file naming one is refused outright.
    shared += "  \"tools.clangTidy.path\": \"/planted/clang-tidy\",\n  \"build.saveAllBeforeBuild\": false\n}\n";
    write(kShared, shared);
    write(kPersonal, R"({"build.msbuild.extraArguments": ["/logger:/planted/evil.dll"]})");
    write(kAppFile, R"({"build.msbuild.extraArguments": ["/v:minimal"]})");

    const auto store = open(false);
    auto snapshot = store->snapshot();
    for (const KeySpec* spec : gated) {
        EXPECT_NE(snapshot->source(spec->key), Scope::ProjectShared) << spec->key;
        EXPECT_NE(snapshot->source(spec->key), Scope::ProjectPersonal) << spec->key;
        EXPECT_NE(std::ranges::find(snapshot->ignoredUntilTrusted(), spec->key), snapshot->ignoredUntilTrusted().end())
            << spec->key;
    }
    EXPECT_EQ(snapshot->get(key::buildMsbuildExtraArguments), StringList{"/v:minimal"});
    EXPECT_EQ(snapshot->get(key::toolsClangTidyPath), "");
    EXPECT_FALSE(snapshot->get(key::buildSaveAllBeforeBuild)); // Keys that run nothing apply at once.
    EXPECT_TRUE(std::ranges::any_of(snapshot->diagnostics(), [](const Diagnostic& diagnostic) {
        return diagnostic.key == "build.msbuild.extraArguments" && diagnostic.severity == Severity::Warning &&
               diagnostic.message.find("trusted") != std::string::npos;
    }));

    std::vector<std::string> changed;
    const Subscription subscription = store->subscribe([&](const SettingsChange& change) { changed = change.keys; });
    store->setProjectTrusted(true);
    snapshot = store->snapshot();
    EXPECT_TRUE(snapshot->trusted());
    EXPECT_EQ(snapshot->get(key::buildMsbuildExtraArguments), StringList{"/logger:/planted/evil.dll"});
    EXPECT_EQ(snapshot->source(key::buildMsbuildExtraArguments.name), Scope::ProjectPersonal);
    EXPECT_TRUE(snapshot->ignoredUntilTrusted().empty());
    EXPECT_EQ(snapshot->get(key::toolsClangTidyPath), ""); // Still Application-only.
    EXPECT_NE(std::ranges::find(changed, "build.msbuild.extraArguments"), changed.end());

    store->setProjectTrusted(false);
    EXPECT_EQ(store->snapshot()->get(key::buildMsbuildExtraArguments), StringList{"/v:minimal"});
}

TEST_F(StoreTest, LiveReloadThroughTheFileWatcher) {
    write(kShared, R"({"schemaVersion": 1, "editor.tabSize": 2})");
    frost::sched::SimExecutor sim;
    frost::io::MemFsWatcher watcher(fs, sim);
    const auto store = open();
    ASSERT_EQ(store->watchDirectories(), (std::vector<std::string>{"/home/u/.config/frostide", "/work/app/.frost"}));

    frost::io::WatchOptions options;
    options.recursive = false;
    auto watchHandle = sim.spawn(frost::sched::Lane::UserVisible, watcher.watch("/work/app/.frost", options));
    auto watch = watchHandle.join();
    ASSERT_TRUE(watch) << watch.error().describe();

    std::vector<std::vector<std::string>> heard;
    const Subscription subscription =
        store->subscribe([&](const SettingsChange& change) { heard.push_back(change.keys); });
    std::stop_source stop;
    auto follow = sim.spawn(frost::sched::Lane::UserVisible, store->follow(**watch, stop.get_token()));

    // An external edit (another editor, git checkout) arrives through the watcher.
    ASSERT_TRUE(fs.writeFileAtomic(kShared, R"({"schemaVersion": 1, "editor.tabSize": 6, "editor.softWrap": true})"));
    sim.runFor(1s);
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 6);
    EXPECT_TRUE(store->snapshot()->get(key::editorSoftWrap));
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(heard[0], (std::vector<std::string>{"editor.softWrap", "editor.tabSize"}));

    // A half-typed edit keeps the last good values and reports the error.
    ASSERT_TRUE(fs.writeFileAtomic(kShared, R"({"schemaVersion": 1, "editor.tabSize": )"));
    sim.runFor(1s);
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 6);
    EXPECT_TRUE(std::ranges::any_of(store->snapshot()->diagnostics(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::Error && diagnostic.path == kShared;
    }));

    // Deleting the file drops its values.
    ASSERT_TRUE(fs.removeFile(kShared));
    sim.runFor(1s);
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 4);
    EXPECT_TRUE(store->snapshot()->diagnostics().empty());

    // The store's own write is applied at once; the watcher's echo then changes nothing.
    const std::size_t before = heard.size();
    ASSERT_TRUE(store->set(Scope::ProjectShared, key::editorTabSize, std::int64_t{5}));
    EXPECT_EQ(heard.size(), before + 1);
    sim.runFor(1s);
    EXPECT_EQ(heard.size(), before + 1);
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 5);

    stop.request_stop();
    sim.runUntilIdle();
    ASSERT_TRUE(follow.ready());
    const auto ended = follow.join();
    ASSERT_FALSE(ended);
    EXPECT_EQ(ended.error().code(), ErrorCode::Cancelled);
    watch->reset();
    sim.runUntilIdle();
}

TEST_F(StoreTest, ApplyChangesFollowsTheSinglePathRules) {
    const auto store = open();
    write(kShared, R"({"editor.tabSize": 7})");
    using frost::io::FileChange;
    using frost::io::FileChangeKind;
    // Unrelated paths and a directory's own Modified are ignored.
    EXPECT_TRUE(
        store
            ->applyChanges(
                {.changes = {FileChange{FileChangeKind::Modified, "/work/app/src/a.cpp", {}, {}},
                             FileChange{
                                 FileChangeKind::Modified, "/work/app/.frost", {}, frost::io::FileType::Directory}},
                 .sequence = 1,
                 .mode = frost::io::WatchMode::Native})
            .empty());
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 4);
    // A Rescan of an ancestor (the project root's recursive watch overflowed) reloads.
    EXPECT_EQ(store->applyChanges({.changes = {FileChange{FileChangeKind::Rescan, "/work/app", {}, {}}},
                                   .sequence = 2,
                                   .mode = frost::io::WatchMode::Native}),
              std::vector<std::string>{"editor.tabSize"});
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 7);
}

TEST_F(StoreTest, ValidationOnSaveReportsAndKeepsTheBytes) {
    const auto store = open();
    // What the editor saved, byte for byte (SaveManager wrote it); validation only reads it.
    const std::string saved =
        "{\n  \"schemaVersion\": 1,\n  \"editor.tabSize\": \"wide\",\n  \"editor.fontSize\": 18,\n  \"mine\": 1\n}";
    write(kAppFile, saved);
    const auto check = store->checkSavedFile(kAppFile, saved);
    ASSERT_TRUE(check.has_value());
    EXPECT_EQ(check->scope, Scope::Application);
    ASSERT_EQ(check->diagnostics.size(), 1U);
    EXPECT_EQ(check->diagnostics[0].key, "editor.tabSize");
    EXPECT_EQ(check->diagnostics[0].line, 3U);
    ASSERT_EQ(check->notifications.size(), 1U);
    EXPECT_EQ(check->notifications[0].severity, Severity::Error);
    EXPECT_EQ(check->notifications[0].title, "Settings file has errors: settings.json");
    EXPECT_EQ(check->notifications[0].message,
              "/home/u/.config/frostide/settings.json:3:3: error: editor.tabSize: must be an integer");
    EXPECT_EQ(read(kAppFile), saved);
    EXPECT_EQ(store->snapshot()->get(key::editorFontSize), 18); // The valid parts apply at once.

    EXPECT_FALSE(store->checkSavedFile("/work/app/src/main.cpp", "int main() {}").has_value());
    const auto clean = store->checkSavedFile(kShared, R"({"schemaVersion": 1, "editor.tabSize": 2})");
    ASSERT_TRUE(clean.has_value());
    EXPECT_TRUE(clean->diagnostics.empty());
    EXPECT_TRUE(clean->notifications.empty());

    EXPECT_EQ(validateSettingsText(builtinRegistry(), Scope::Application, "x.json", "{").size(), 1U);
}

TEST_F(StoreTest, StandInActionsPrepareTheScopeFiles) {
    ASSERT_EQ(settingsActions().size(), 3U);
    const auto store = open();
    for (const SettingsActionInfo& action : settingsActions()) {
        EXPECT_TRUE(action.id.starts_with("settings.")) << action.id;
        EXPECT_FALSE(action.name.empty());
        const auto path = store->prepareForEditing(action.scope);
        ASSERT_TRUE(path) << path.error().describe();
        EXPECT_EQ(*path, store->pathOf(action.scope));
        EXPECT_EQ(read(*path), "{\n  \"schemaVersion\": 1\n}\n");
    }
    EXPECT_EQ(read("/work/app/.frost/.gitignore"), kFrostGitignoreText);
    // An existing file is opened as it is.
    write(kAppFile, "{ \"editor.tabSize\": 3 }");
    ASSERT_TRUE(store->prepareForEditing(Scope::Application));
    EXPECT_EQ(read(kAppFile), "{ \"editor.tabSize\": 3 }");
    EXPECT_EQ(store->prepareForEditing(Scope::FileLevel).error().code(), ErrorCode::InvalidArgument);

    const auto noProject = open(false, false);
    EXPECT_EQ(noProject->prepareForEditing(Scope::ProjectShared).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(noProject->set(Scope::ProjectShared, key::editorTabSize, std::int64_t{2}).error().code(),
              ErrorCode::NotFound);
}

TEST_F(StoreTest, SwitchingProjects) {
    write(kShared, R"({"editor.tabSize": 2})");
    write("/work/other/.frost/settings.json", R"({"editor.tabSize": 3})");
    const auto store = open(false, false);
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 4);
    ASSERT_TRUE(store->setProject(ProjectLayout::forRoot(kRoot, PathStyle::Posix).value()));
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 2);
    EXPECT_EQ(store->scopeOf("/work/app/.frost/./settings.json"), Scope::ProjectShared);
    ASSERT_TRUE(store->setProject(ProjectLayout::forRoot("/work/other", PathStyle::Posix).value()));
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 3);
    ASSERT_TRUE(store->setProject(std::nullopt));
    EXPECT_EQ(store->snapshot()->get(key::editorTabSize), 4);
    EXPECT_EQ(store->pathOf(Scope::ProjectShared), "");
}

TEST(SettingsPaths, ApplicationSettingsFile) {
    const auto lookup = [](std::map<std::string, std::string> variables) -> EnvironmentLookup {
        return [variables = std::move(variables)](std::string_view name) -> std::optional<std::string> {
            const auto it = variables.find(std::string(name));
            return it != variables.end() ? std::optional(it->second) : std::nullopt;
        };
    };
    EXPECT_EQ(
        applicationSettingsFileFor(lookup({{"APPDATA", "C:\\Users\\u\\AppData\\Roaming"}}), PathStyle::Windows).value(),
        "C:\\Users\\u\\AppData\\Roaming\\FrostIDE\\settings.json");
    EXPECT_EQ(applicationSettingsFileFor(lookup({}), PathStyle::Windows).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(applicationSettingsFileFor(lookup({{"XDG_CONFIG_HOME", "/x/cfg"}, {"HOME", "/home/u"}}), PathStyle::Posix)
                  .value(),
              "/x/cfg/frostide/settings.json");
    EXPECT_EQ(
        applicationSettingsFileFor(lookup({{"XDG_CONFIG_HOME", "relative"}, {"HOME", "/home/u"}}), PathStyle::Posix)
            .value(),
        "/home/u/.config/frostide/settings.json");
    // The real environment always has one on the supported platforms.
    const auto real = applicationSettingsFile();
    ASSERT_TRUE(real) << real.error().describe();
    EXPECT_TRUE(frost::io::isAbsolutePath(*real));
}

} // namespace
