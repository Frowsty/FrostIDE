// SPDX-License-Identifier: Apache-2.0
// Exercises editor and Problems publication through the bundled snapshot validator.

#include "frost/app/document_editor.hpp"
#include "frost/io/async_process.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "workbench_rig.hpp"

#include <algorithm>
#include <atomic>
#include <semaphore>

namespace {
using namespace frost;
using namespace frost::app::test;
using namespace std::chrono_literals;

class ValidatorLauncher final : public io::IProcessLauncher {
public:
    explicit ValidatorLauncher(const WorkDir& dir) : m_missingExecutable(dir.join("missing-validator.exe")) {}

    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (io::fileName(spec.executable) != "frost-snapshot-validator" &&
            io::fileName(spec.executable) != "frost-snapshot-validator.exe")
            return io::realProcessLauncher().launch(spec);
        const int attempt = ++launches;
        if (attempt == holdAttempt && !release.try_acquire_for(4s)) {
            gateTimedOut = true;
            return fail(ErrorCode::Timeout, "The validator test gate was not released");
        }
        if (missing) {
            auto absent = spec;
            absent.executable = m_missingExecutable;
            return io::realProcessLauncher().launch(absent);
        }
        return io::realProcessLauncher().launch(spec);
    }

    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        return io::realProcessLauncher().launchAsync(spec, context);
    }

    std::atomic<int> launches = 0;
    std::atomic<bool> missing = false;
    std::atomic<bool> gateTimedOut = false;
    int holdAttempt = 0;
    std::binary_semaphore release{0};

private:
    std::string m_missingExecutable;
};

bool replaceBuffer(app::DocumentEditorView& view, std::string_view bytes) {
    auto& editor = view.editor();
    auto change = editor.document().replace({ByteOffset{0}, editor.editor().text().size()}, bytes, {});
    return change && editor.followExternalChange(*change).has_value();
}

std::optional<ui::Problem> problem(const WorkbenchRig& rig, std::string_view path, std::string_view code) {
    const auto& rows = rig.workbench->problems()->model()->problems();
    const auto found = std::ranges::find_if(rows, [&](const ui::Problem& row) {
        return row.source == ui::DiagnosticSource::Live && row.diagnostic.path == path &&
               row.diagnostic.diagnostic.code == code;
    });
    return found == rows.end() ? std::nullopt : std::optional(*found);
}

std::string validationState(const WorkbenchRig& rig, const ValidatorLauncher& launcher) {
    std::string state = "Worker launches: " + std::to_string(launcher.launches.load());
    for (const auto& entry : rig.window->notifications().log())
        state += "\n" + entry.notification.title.toStdString() + " " + entry.notification.text.toStdString();
    for (const auto& row : rig.workbench->problems()->model()->problems())
        state +=
            "\n" + row.diagnostic.path + " " + row.diagnostic.diagnostic.code + " " + row.diagnostic.diagnostic.message;
    return state;
}

TEST(WorkbenchSnapshotValidation, UnsavedCMakeAndJsonReachEditorAndProblems) {
    WorkDir dir("snapshot-validator");
    constexpr std::string_view cmake = "add_library(car STATIC car.cpp)\n";
    dir.write("targets.cmake", cmake);
    dir.write("CMakePresets.json", R"({"version":3})");
    ValidatorLauncher launcher(dir);
    WorkbenchRig rig(dir, {.launcher = &launcher});

    const auto cmakePath = dir.join("targets.cmake");
    ASSERT_TRUE(rig.workbench->openPath(cmakePath));
    auto* cmakeView = rig.view();
    ASSERT_NE(cmakeView, nullptr);
    ASSERT_TRUE(replaceBuffer(*cmakeView, std::string(cmake) + "add_library(car STATIC other.cpp)\n"));
    ASSERT_TRUE(waitFor([&] { return problem(rig, cmakePath, "cmake.duplicate-target").has_value(); }))
        << validationState(rig, launcher);
    const auto duplicate = problem(rig, cmakePath, "cmake.duplicate-target");
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->diagnostic.diagnostic.line, 2);
    EXPECT_EQ(duplicate->diagnostic.diagnostic.column, 1);
    const auto cmakeMarks = cmakeView->editor().diagnostics();
    ASSERT_EQ(cmakeMarks.size(), 1U);
    EXPECT_EQ(cmakeMarks.front().code, "cmake.duplicate-target");
    EXPECT_EQ(cmakeMarks.front().range.begin.value(), cmake.size());
    EXPECT_EQ(dir.read("targets.cmake"), cmake);

    const auto jsonPath = dir.join("CMakePresets.json");
    ASSERT_TRUE(rig.workbench->openPath(jsonPath));
    auto* jsonView = rig.view(1);
    ASSERT_NE(jsonView, nullptr);
    constexpr std::string_view json = "{\"label\":\"\xf0\x9f\x8d\x80\", \"version\":\"bad\"}";
    ASSERT_TRUE(replaceBuffer(*jsonView, json));
    ASSERT_TRUE(waitFor([&] { return problem(rig, jsonPath, "schema").has_value(); }))
        << validationState(rig, launcher);
    const auto shape = problem(rig, jsonPath, "schema");
    ASSERT_TRUE(shape);
    const auto value = json.find("\"bad\"");
    EXPECT_EQ(shape->diagnostic.diagnostic.line, 1);
    EXPECT_EQ(shape->diagnostic.diagnostic.column, static_cast<int>(value) - 1); // One UTF-16 surrogate pair.
    const auto jsonMarks = jsonView->editor().diagnostics();
    ASSERT_EQ(jsonMarks.size(), 1U);
    EXPECT_EQ(jsonMarks.front().range.begin.value(), value);
    EXPECT_EQ(jsonMarks.front().range.end.value(), value + 5);
    EXPECT_EQ(dir.read("CMakePresets.json"), R"({"version":3})");
    EXPECT_EQ(launcher.launches.load(), 2);

    ASSERT_TRUE(replaceBuffer(*jsonView, R"({"version":3})"));
    EXPECT_FALSE(problem(rig, jsonPath, "schema"));
    EXPECT_TRUE(jsonView->editor().diagnostics().empty());
    EXPECT_TRUE(problem(rig, cmakePath, "cmake.duplicate-target"));
}

TEST(WorkbenchSnapshotValidation, MissingHelperClearsOwnedProblemsAndReportsUnavailable) {
    WorkDir dir("snapshot-validator-unavailable");
    dir.write("targets.cmake", "add_library(car STATIC car.cpp)\nadd_library(car STATIC other.cpp)\n");
    dir.write("CMakePresets.json", R"({"version":"bad"})");
    ValidatorLauncher launcher(dir);
    WorkbenchRig rig(dir, {.launcher = &launcher});
    const auto cmakePath = dir.join("targets.cmake"), jsonPath = dir.join("CMakePresets.json");
    ASSERT_TRUE(rig.workbench->openPath(cmakePath));
    ASSERT_TRUE(waitFor([&] { return problem(rig, cmakePath, "cmake.duplicate-target").has_value(); }))
        << validationState(rig, launcher);
    auto* cmakeView = rig.view();
    ASSERT_NE(cmakeView, nullptr);
    ui::EditorDiagnostic independent;
    independent.message = "Another live diagnostic";
    independent.code = "independent";
    independent.line = 1;
    independent.column = 1;
    cmakeView->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {independent});
    launcher.missing = true;
    ASSERT_TRUE(rig.workbench->openPath(jsonPath));
    ASSERT_TRUE(waitFor([&] {
        return std::ranges::any_of(rig.window->notifications().log(), [](const ui::NotificationEntry& entry) {
            return entry.notification.title == QStringLiteral("Live document validation unavailable");
        });
    }));
    EXPECT_EQ(launcher.launches.load(), 2);
    EXPECT_FALSE(problem(rig, jsonPath, "schema"));
    EXPECT_FALSE(problem(rig, cmakePath, "cmake.duplicate-target"));
    const auto diagnostics = cmakeView->editor().diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().code, "independent");
}

TEST(WorkbenchSnapshotValidation, SupersededWorkerResponseNeverPublishes) {
    WorkDir dir("snapshot-validator-late");
    dir.write("CMakePresets.json", R"({"version":"old"})");
    ValidatorLauncher launcher(dir);
    launcher.holdAttempt = 1;
    bool stalePublished = false;
    WorkbenchRig rig(dir, {.launcher = &launcher});
    const auto path = dir.join("CMakePresets.json");
    auto* model = rig.workbench->problems()->model();
    QObject watchLifetime;
    QObject::connect(model, &QAbstractItemModel::modelReset, &watchLifetime,
                     [&] { stalePublished |= problem(rig, path, "schema").has_value(); });
    ASSERT_TRUE(rig.workbench->openPath(path));
    ASSERT_TRUE(waitFor([&] { return launcher.launches.load() == 1; }));
    auto* view = rig.view();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(replaceBuffer(*view, R"({"version":)"));
    launcher.release.release();
    ASSERT_TRUE(waitFor([&] { return problem(rig, path, "syntax").has_value(); })) << validationState(rig, launcher);
    EXPECT_EQ(launcher.launches.load(), 2);
    EXPECT_FALSE(launcher.gateTimedOut.load());
    EXPECT_FALSE(stalePublished);
    const auto diagnostics = view->editor().diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().code, "syntax");
}

TEST(WorkbenchSnapshotValidation, ClosedDocumentResponseCannotReachReopenedTab) {
    WorkDir dir("snapshot-validator-reopen");
    dir.write("CMakePresets.json", R"({"version":"old"})");
    ValidatorLauncher launcher(dir);
    launcher.holdAttempt = 1;
    bool stalePublished = false;
    WorkbenchRig rig(dir, {.launcher = &launcher});
    const auto path = dir.join("CMakePresets.json");
    auto* model = rig.workbench->problems()->model();
    QObject watchLifetime;
    QObject::connect(model, &QAbstractItemModel::modelReset, &watchLifetime,
                     [&] { stalePublished |= problem(rig, path, "schema").has_value(); });
    ASSERT_TRUE(rig.workbench->openPath(path));
    ASSERT_TRUE(waitFor([&] { return launcher.launches.load() == 1; }));
    ASSERT_TRUE(rig.window->tabs().close(rig.window->tabs().indexOf(path)));
    dir.write("CMakePresets.json", R"({"version":)");
    ASSERT_TRUE(rig.workbench->openPath(path));
    launcher.release.release();
    ASSERT_TRUE(waitFor([&] { return problem(rig, path, "syntax").has_value(); })) << validationState(rig, launcher);
    EXPECT_EQ(launcher.launches.load(), 2);
    EXPECT_FALSE(launcher.gateTimedOut.load());
    EXPECT_FALSE(stalePublished);
}
} // namespace
