// SPDX-License-Identifier: Apache-2.0
//
// The real thread samplers (frost/diag/thread_sampler.hpp) and module identities
// (frost/diag/module_identity.hpp), checked against independent oracles: llvm-symbolizer (run
// through frost::io's launcher) must name a blocked thread's call chain in order from the
// sampled frames; on Windows the sampled return addresses must equal the ones the thread
// captured itself (RtlCaptureStackBackTrace); the recorded PDB GUID and age must match
// `llvm-readobj --coff-debug-directory`, the ELF build-id `llvm-readelf -n`. The heap and
// loader churn case is in diag_stress_test.cpp.

#include "frost/diag/module_identity.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/path.hpp"

#include "diag_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;

/* The blocked thread spins on g_release through the __atomic builtins, which compile to plain
   loads even without optimisation, so every sample lands in blockedLevel3 itself and never in
   an out-of-line std::atomic member (as it would in a Debug build). */
bool g_release = false;
std::atomic<int> g_entered{0};
#ifdef _WIN32
// The return addresses the blocked thread captured itself, from inside blockedLevel3.
void* g_ownFrames[8] = {};
unsigned g_ownCount = 0;
#endif

[[clang::noinline]] void blockedLevel3() {
#ifdef _WIN32
    g_ownCount = RtlCaptureStackBackTrace(0, 8, g_ownFrames, nullptr);
#endif
    g_entered.store(1, std::memory_order_release);
    while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) {
        // Spin: every sample lands in this loop.
    }
}

[[clang::noinline]] void blockedLevel2() {
    blockedLevel3();
    g_entered.fetch_add(0, std::memory_order_relaxed); // Keeps the call from being a tail call.
}

[[clang::noinline]] void blockedLevel1() {
    blockedLevel2();
    g_entered.fetch_add(0, std::memory_order_relaxed);
}

// The function names llvm-symbolizer gives for `frames` (relative to their modules).
std::vector<std::string> symbolize(const std::vector<std::uintptr_t>& frames) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        auto module = frost::diag::moduleForAddress(frames[i]);
        if (!module) {
            names.emplace_back("<no module>");
            continue;
        }
        // Return addresses point after the call; look up the byte before (as symbolizers do).
        // PE: an RVA (--relative-address). ELF: the offset from the load bias is the virtual
        // address llvm-symbolizer takes.
        const std::uintptr_t offset = frames[i] - module->base - (i == 0 ? 0 : 1);
        std::vector<std::string> arguments{"--obj=" + module->path, "--functions=linkage", "--demangle"};
#ifdef _WIN32
        arguments.emplace_back("--relative-address");
#endif
        arguments.push_back(std::format("0x{:x}", offset));
        const frost::test::ChildRun run = frost::test::runChild(frost::io::normalizePath(FROST_LLVM_SYMBOLIZER).value(),
                                                                arguments, {}, {}, 60s, false);
        const std::size_t end = run.output.find_first_of("\r\n");
        names.push_back(run.output.substr(0, end));
    }
    return names;
}

TEST(ThreadSampler, SampledFramesSymbolizeToTheBlockedChain) {
    __atomic_store_n(&g_release, false, __ATOMIC_RELEASE);
    g_entered = 0;
    std::unique_ptr<frost::diag::IThreadSampler> sampler;
    std::atomic<bool> ready{false};
    std::thread target([&] {
        auto created = frost::diag::createSamplerForCurrentThread();
        if (created) {
            sampler = std::move(*created);
        }
        ready.store(true, std::memory_order_release);
        blockedLevel1();
    });
    while (!ready.load(std::memory_order_acquire) || g_entered.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    ASSERT_NE(sampler, nullptr);
    frost::diag::StackSample sample;
    const auto taken = sampler->sample(sample);
    __atomic_store_n(&g_release, true, __ATOMIC_RELEASE);
    target.join();
    ASSERT_TRUE(taken.has_value()) << taken.error().describe();
    ASSERT_TRUE(sample.captured());
    ASSERT_GE(sample.count, 3u);

    const std::vector<std::uintptr_t> frames(sample.frames.begin(), sample.frames.begin() + 3);
    const std::vector<std::string> names = symbolize(frames);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_TRUE(names[0].contains("blockedLevel3")) << names[0];
    EXPECT_TRUE(names[1].contains("blockedLevel2")) << names[1];
    EXPECT_TRUE(names[2].contains("blockedLevel1")) << names[2];
#ifdef _WIN32
    // Frame 0 of the thread's own capture is inside blockedLevel3 (the call's return address);
    // the callers' return addresses must be the same in both.
    ASSERT_GE(g_ownCount, 3u);
    EXPECT_EQ(sample.frames[1], reinterpret_cast<std::uintptr_t>(g_ownFrames[1]));
    EXPECT_EQ(sample.frames[2], reinterpret_cast<std::uintptr_t>(g_ownFrames[2]));
#endif
}

// The tool's output for this test binary, read with llvm-readobj / llvm-readelf.
std::string readObject(const std::vector<std::string>& arguments) {
    const frost::test::ChildRun run =
        frost::test::runChild(frost::io::normalizePath(FROST_LLVM_READOBJ).value(), arguments, {}, {}, 60s, false);
    EXPECT_EQ(run.exitCode, 0) << run.output << run.error;
    return run.output;
}

#ifndef _WIN32
std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
#endif

TEST(ModuleIdentity, MatchesLlvmReadobj) {
    auto module = frost::diag::moduleForAddress(reinterpret_cast<std::uintptr_t>(&blockedLevel1));
    ASSERT_TRUE(module.has_value()) << module.error().describe();
    EXPECT_LT(reinterpret_cast<std::uintptr_t>(&blockedLevel1) - module->base, module->size);
#ifdef _WIN32
    ASSERT_EQ(module->kind, frost::diag::ModuleIdentity::Kind::Pdb70);
    const std::string text = readObject({"--coff-debug-directory", module->path});
    // llvm-readobj prints "PDBGUID: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" and "PDBAge: N".
    EXPECT_TRUE(text.contains("{" + module->guid + "}")) << module->guid << "\n" << text;
    EXPECT_TRUE(text.contains(std::format("PDBAge: {}", module->age))) << text;
    EXPECT_TRUE(text.contains(module->pdb)) << module->pdb;
#else
    ASSERT_EQ(module->kind, frost::diag::ModuleIdentity::Kind::GnuBuildId);
    const std::string text = readObject({"--elf-output-style=GNU", "-n", module->path});
    // llvm-readelf -n prints "Build ID: <hex>".
    EXPECT_TRUE(lower(text).contains("build id: " + module->buildId)) << module->buildId << "\n" << text;
#endif
}

TEST(ModuleIdentity, TableCachesModulesAndRejectsUnmappedAddresses) {
    frost::diag::ModuleTable table;
    const int first = table.indexOf(reinterpret_cast<std::uintptr_t>(&blockedLevel1));
    const int second = table.indexOf(reinterpret_cast<std::uintptr_t>(&blockedLevel2));
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 0);
    EXPECT_EQ(table.indexOf(0x10), -1);
    EXPECT_EQ(table.modules().size(), 1u);
}

} // namespace
