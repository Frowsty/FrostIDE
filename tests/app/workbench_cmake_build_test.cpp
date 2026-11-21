// SPDX-License-Identifier: Apache-2.0
// Exercises the Workbench's Linux build lifecycle with real CMake.

#include "frost/build/cmake_service.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_menus.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"

#include "workbench_rig.hpp"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QToolButton>

#include <chrono>

namespace {

using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

TEST(WorkbenchCMakeBuild, SaveAllRunsBeforeABuild) {
    WorkDir dir("cmake-save-before-build");
    dir.write("CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.21)
project(SaveBeforeBuild NONE)
add_custom_target(probe ALL
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/input.txt"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/saved-during-build.txt"
    VERBATIM)
)");
    dir.write("input.txt", "original\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 60s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("input.txt")));
    ASSERT_NE(rig.view(), nullptr);
    ASSERT_TRUE(rig.view()->editor().typeText("edited\n"));
    ASSERT_TRUE(rig.window->tabs().isDirty(0));
    rig.window->menus().refresh();
    auto* build = rig.window->menus().action(frost::ui::build_actions::kBuildSolution);
    ASSERT_NE(build, nullptr);
    ASSERT_TRUE(waitFor([&] {
        rig.window->menus().refresh();
        return build->isEnabled();
    }));
    build->trigger();
    ASSERT_TRUE(waitFor(
        [&] {
            return dir.read("saved-during-build.txt") != "<unreadable>" &&
                   service->state() == frost::build::BuildState::Idle;
        },
        60s));
    EXPECT_EQ(dir.read("saved-during-build.txt"), "edited\noriginal\n");
    EXPECT_FALSE(rig.window->tabs().isDirty(0));
    EXPECT_TRUE(rig.window->toolWindows().isVisible(frost::app::kBuildToolWindow));
}

TEST(WorkbenchCMakeBuild, SwitchingProjectsDuringBuildKeepsTheCurrentProjectAndTabs) {
    WorkDir dir("cmake-busy-build");
    WorkDir other("cmake-other-project");
    dir.write("input.txt", "original\n");
    dir.write("CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.21)
project(HoldBuild NONE)
add_custom_target(probe ALL COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/hold.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
)");
    dir.write("hold.cmake", R"(file(WRITE started "started")
foreach(attempt RANGE 1 150)
    if(EXISTS release)
        file(WRITE finished "finished")
        return()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()
message(FATAL_ERROR "Test never released the build")
)");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(frost::app::addRecentProject(*rig.settings, other.path()));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(waitFor([&] { return service->model() && service->state() == frost::build::BuildState::Idle; }, 60s));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("input.txt")));
    rig.window->menus().refresh();
    auto* build = rig.window->menus().action(frost::ui::build_actions::kBuildSolution);
    ASSERT_NE(build, nullptr);
    ASSERT_TRUE(waitFor([&] {
        rig.window->menus().refresh();
        return build->isEnabled();
    }));
    build->trigger();
    ASSERT_TRUE(waitFor([&] { return dir.read("started") == "started"; }, 60s));

    auto* switcher = rig.window->mainToolbar().projectButton()->menu();
    ASSERT_NE(switcher, nullptr);
    QMetaObject::invokeMethod(switcher, "aboutToShow", Qt::DirectConnection);
    QAction* otherAction = nullptr;
    for (auto* action : switcher->actions())
        if (action->data().toString().toStdString() == other.path())
            otherAction = action;
    ASSERT_NE(otherAction, nullptr);
    otherAction->trigger();
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_FALSE(rig.workbench->openPath(other.path()));
    EXPECT_FALSE(rig.workbench->closeProject());
    EXPECT_EQ(rig.workbench->projectRoot(), dir.path());
    EXPECT_EQ(rig.window->tabs().count(), 1);
    ASSERT_NE(rig.view(), nullptr);
    EXPECT_EQ(rig.view()->path(), dir.join("input.txt"));

    dir.write("release", "");
    ASSERT_TRUE(waitFor([&] { return service->state() == frost::build::BuildState::Idle; }, 60s));
    EXPECT_EQ(dir.read("finished"), "finished");
}

} // namespace
