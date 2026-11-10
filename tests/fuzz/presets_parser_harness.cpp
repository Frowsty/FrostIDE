// SPDX-License-Identifier: Apache-2.0
//
// Fuzz harness for frost::project::loadCMakePresets (FND-14, PLAN.md Appendix C):
//   - populates an in-memory workspace (MemFs) with CMakePresets.json, optional
//     CMakeUserPresets.json and optional included preset files derived from the fuzz input;
//   - evaluates inheritance, macro expansion ($env{}, $penv{}, ${sourceDir}, ${presetName},
//     etc.) and condition objects under both Windows and Linux HostOs;
//   - inspects configure, build, test, package and workflow presets and warnings.

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_presets.hpp"

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace frost::fuzz {
namespace {

void inspectPresets(const project::CMakePresets& presets) {
    for (const auto& cfg : presets.configurePresets) {
        (void)presets.findConfigurePreset(cfg.name);
        (void)presets.buildPresetsFor(cfg.name);
        (void)presets.testPresetsFor(cfg.name);
        (void)cfg.label();
        (void)cfg.binaryDir;
        (void)cfg.generator;
        (void)cfg.cacheVariables.size();
        (void)cfg.environment.size();
    }
    for (const auto& bld : presets.buildPresets) {
        (void)presets.findBuildPreset(bld.name);
        (void)bld.label();
        (void)bld.configurePreset;
        (void)bld.targets.size();
    }
    for (const auto& tst : presets.testPresets) {
        (void)tst.name;
        (void)tst.configurePreset;
    }
    (void)presets.packagePresets.size();
    (void)presets.workflowPresets.size();
    (void)presets.files.size();
    const project::CMakePresetListing listing = presets.listing();
    (void)project::formatCMakePresetListing(listing);
}

} // namespace

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());

    ManualClock clock;
    io::MemFs fs(clock, io::PathStyle::Posix);
    (void)fs.createDirectories("/workspace");

    constexpr std::string_view kUserSplit = "\n---USER---\n";
    constexpr std::string_view kIncSplit = "\n---INC---\n";

    std::string_view mainPart = text;
    std::string_view userPart;
    std::string_view incPart;

    if (const std::size_t userPos = mainPart.find(kUserSplit); userPos != std::string_view::npos) {
        userPart = mainPart.substr(userPos + kUserSplit.size());
        mainPart = mainPart.substr(0, userPos);
        if (const std::size_t incPos = userPart.find(kIncSplit); incPos != std::string_view::npos) {
            incPart = userPart.substr(incPos + kIncSplit.size());
            userPart = userPart.substr(0, incPos);
        }
    } else if (const std::size_t incPos = mainPart.find(kIncSplit); incPos != std::string_view::npos) {
        incPart = mainPart.substr(incPos + kIncSplit.size());
        mainPart = mainPart.substr(0, incPos);
    }

    (void)fs.writeFileAtomic("/workspace/CMakePresets.json", mainPart);
    if (!userPart.empty()) {
        (void)fs.writeFileAtomic("/workspace/CMakeUserPresets.json", userPart);
    }
    if (!incPart.empty()) {
        (void)fs.writeFileAtomic("/workspace/common.json", incPart);
    }

    const auto env = [](std::string_view name) -> std::optional<std::string> {
        if (name == "FROST_SDK") {
            return "/sdk";
        }
        if (name == "BUILD_ROOT") {
            return "/workspace/out";
        }
        return std::nullopt;
    };

    const project::CMakePresetsContext linuxCtx{.hostSystemName = "Linux", .environment = env};
    if (auto linuxPresets = project::loadCMakePresets(fs, "/workspace", linuxCtx);
        linuxPresets && linuxPresets->has_value()) {
        inspectPresets(**linuxPresets);
    }
    const project::CMakePresetsContext winCtx{.hostSystemName = "Windows", .environment = env};
    if (auto winPresets = project::loadCMakePresets(fs, "/workspace", winCtx); winPresets && winPresets->has_value()) {
        inspectPresets(**winPresets);
    }
    return {};
}

} // namespace frost::fuzz
