// SPDX-License-Identifier: Apache-2.0
// LUA-12: explicit standalone Lua framework provider; process ownership remains in TestRunner.
#pragma once
#include "frost/run/test_runner.hpp"

#include <functional>
#include <stop_token>
class QWidget;
namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io
namespace frost::project {
class TrustStore;
}
namespace frost::app {
struct LuaUnitTestsSource {
    run::RunConfiguration configuration;
    std::string trustRoot;
    std::uint64_t trustRevision{};
    std::stop_token lifetime;
};
struct LuaUnitTestsContext {
    io::IFileSystem* fs{};
    io::IProcessLauncher* launcher{};
    project::TrustStore* trust{};
    std::string scratchDirectory;
    std::string discoveryBridge;
    std::function<Result<LuaUnitTestsSource>()> source;
    std::function<Result<void>(const run::RunConfiguration&)> prepare;
    std::function<Result<void>(run::RunConfiguration)> debug;
    std::function<void(const std::string&, int)> openLocation;
    std::function<void()> testsChanged;
};
[[nodiscard]] QWidget* createLuaUnitTestsWidget(QWidget* parent, LuaUnitTestsContext context);
// Concrete provider lookup only, including one unambiguous tab page; never object-name casts.
[[nodiscard]] std::vector<run::GutterTestCase> luaUnitTestsGutterCases(QWidget*, std::string_view path);
[[nodiscard]] Result<void> runLuaGutterCase(QWidget*, const run::GutterTestCase&, bool debug);
} // namespace frost::app
