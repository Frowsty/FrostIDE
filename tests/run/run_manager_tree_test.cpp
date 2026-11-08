// SPDX-License-Identifier: Apache-2.0
//
// RUN-1 acceptance (label integration): Stop leaves no descendant process. run_tree_child builds a
// real tree of seven processes (the program, two children, four grandchildren; on Linux one branch
// in a process group of its own); the test checks with the OS process table (process_table.hpp)
// that the tree is what the children reported, stops the run through RunManager, and checks by
// enumeration again that none of them is left. Also: destroying the manager ends a running tree.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "io/process_env.hpp"
#include "run/process_table.hpp"
#include "run/run_collector.hpp"
#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::run {
namespace {

using namespace std::chrono_literals;

constexpr auto kWait = 60s;          // Generous: the machine may be building in parallel.
constexpr std::size_t kTreeSize = 7; // tree 2 2: 1 + 2 + 4.

std::string treeChildPath() { return io::normalizePath(FROST_RUN_TREE_CHILD).value(); }

std::string treeImage() { return test::lowerAscii(io::fileName(treeChildPath())); }

RunConfiguration treeConfiguration() {
    RunConfiguration configuration;
    configuration.id = "tree";
    configuration.name = "Process tree";
    configuration.type = RunType::Native;
    configuration.program = treeChildPath();
    configuration.arguments = {"tree", "2", "2"};
    return configuration;
}

// The pids in the children's "pid <n>" lines.
std::set<io::ProcessId> reportedPids(const std::string& out) {
    std::set<io::ProcessId> pids;
    std::size_t at = 0;
    while ((at = out.find("pid ", at)) != std::string::npos) {
        at += 4;
        io::ProcessId pid = 0;
        const auto parsed = std::from_chars(out.data() + at, out.data() + out.size(), pid);
        if (parsed.ec == std::errc{} && pid != 0) {
            pids.insert(pid);
        }
    }
    return pids;
}

std::size_t lineCount(const std::string& out) { return static_cast<std::size_t>(std::ranges::count(out, '\n')); }

// Whether `pid` names a process that has not ended (Windows lists ended ones while handles to them stay open).
bool running([[maybe_unused]] const test::OsProcess& process) {
#ifdef _WIN32
    const HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, process.pid);
    if (handle == nullptr) {
        return false;
    }
    const bool alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
#else
    return true; // processTable() already leaves out zombies.
#endif
}

/*
 * Windows gives a console program that starts without a console (as the launcher starts its
 * children) a console host, conhost.exe, which the table lists as the program's child. It belongs to
 * the tree as much as the program's own children, so Stop must end it too.
 */
bool consoleHost([[maybe_unused]] const test::OsProcess& process) {
#ifdef _WIN32
    return process.image == "conhost.exe";
#else
    return false;
#endif
}

// The live processes of the tree: one of `pids`, or below `root` by a fresh enumeration (run_tree_child
// or a console host; on Windows the parent links of ended processes stay in the table).
std::vector<test::OsProcess> survivors(io::ProcessId root, const std::set<io::ProcessId>& pids) {
    const auto table = test::processTable();
    const auto below = test::descendantsOf(root, table);
    std::vector<test::OsProcess> left;
    for (const test::OsProcess& process : table) {
        const bool ours = process.image == treeImage() || consoleHost(process);
        if (ours && (pids.contains(process.pid) || below.contains(process.pid)) && running(process)) {
            left.push_back(process);
        }
    }
    return left;
}

// Starts the tree and waits until all seven processes reported; checks the tree against the OS table.
struct StartedTree {
    RunId id = 0;
    io::ProcessId root = 0;
    std::set<io::ProcessId> pids;  // The reported run_tree_child processes, the root included.
    std::set<io::ProcessId> hosts; // Their console hosts (Windows), found in the process table.
    std::vector<std::unique_ptr<test::ProcessWatch>> watches;

    // Every process of the tree.
    [[nodiscard]] std::set<io::ProcessId> all() const {
        std::set<io::ProcessId> out = pids;
        out.insert(hosts.begin(), hosts.end());
        return out;
    }
};

void startTree(RunManager& manager, test::RunCollector& collector, StartedTree& tree) {
    auto id = manager.start(treeConfiguration(), collector.handlers());
    ASSERT_TRUE(id.has_value()) << id.error().describe();
    tree.id = *id;
    const auto reported = collector.waitUntil(
        [](const test::RunCollector::Snapshot& s) { return lineCount(s.out) >= kTreeSize || s.result.has_value(); },
        kWait);
    ASSERT_FALSE(reported.result.has_value()) << "the tree ended early: " << reported.out << reported.err;
    ASSERT_EQ(reported.out.find("spawn failed"), std::string::npos) << reported.out;
    tree.pids = reportedPids(reported.out);
    ASSERT_EQ(tree.pids.size(), kTreeSize) << reported.out;
    const auto status = manager.status(tree.id);
    ASSERT_TRUE(status.has_value());
    tree.root = status->processId;
    ASSERT_TRUE(tree.pids.contains(tree.root)) << "the program is the root of the tree it reported";
    // Watches opened while every process is known to run, so no id can have been reused.
    for (const io::ProcessId pid : tree.pids) {
        tree.watches.push_back(std::make_unique<test::ProcessWatch>(pid));
        ASSERT_TRUE(tree.watches.back()->valid()) << pid;
    }

    // The oracle: below the program, the OS process table holds exactly the processes that reported
    // themselves, plus (Windows) their console hosts.
    const auto table = test::processTable();
    ASSERT_FALSE(table.empty()) << "the process table could not be read";
    std::set<io::ProcessId> expectedBelow = tree.pids;
    expectedBelow.erase(tree.root);
    const auto below = test::descendantsOf(tree.root, table);
    std::set<io::ProcessId> belowTree;
    for (const test::OsProcess& process : table) {
        if (!below.contains(process.pid)) {
            continue;
        }
        if (process.image == treeImage()) {
            belowTree.insert(process.pid);
        } else if (consoleHost(process)) {
            tree.hosts.insert(process.pid);
        } else {
            ADD_FAILURE() << "an unexpected process below the program: " << process.image << " " << process.pid;
        }
    }
    EXPECT_EQ(belowTree, expectedBelow);
    for (const io::ProcessId pid : tree.hosts) {
        auto watch = std::make_unique<test::ProcessWatch>(pid);
        if (watch->valid()) { // A host may have ended with its console since the snapshot.
            tree.watches.push_back(std::move(watch));
        }
    }
}

// Waits for every watched process to end; kills and reports any that does not (the test's cleanup).
void expectAllEnd(StartedTree& tree) {
    for (const auto& watch : tree.watches) {
        if (!watch->exitsWithin(10s)) {
            ADD_FAILURE() << "a process of the tree is still running";
            watch->kill();
        }
    }
    tree.watches.clear(); // Close the handles, so Windows drops the ended processes from its table.
}

TEST(RunManagerTree, StopLeavesNoDescendantProcess) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector collector;
    StartedTree tree;
    startTree(*manager, collector, tree);
    if (HasFatalFailure()) {
        manager.reset(); // Ends whatever started.
        return;
    }

    manager->stop(tree.id);
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    ASSERT_TRUE(finished.result->ran());
    EXPECT_TRUE(finished.result->stopped);
    EXPECT_EQ(*finished.result->exitCode, io::kTerminatedExitCode);
    EXPECT_FALSE(finished.result->cleanupFailed);
    const auto all = tree.all();
    expectAllEnd(tree);
    const auto left = survivors(tree.root, all);
    EXPECT_TRUE(left.empty()) << left.size() << " process(es) of the tree survived Stop, first "
                              << (left.empty() ? std::string() : left.front().image) << " "
                              << (left.empty() ? 0 : left.front().pid);
    EXPECT_TRUE(manager->waitForIdle(kWait));
}

TEST(RunManagerTree, DestroyingTheManagerEndsARunningTree) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector collector;
    StartedTree tree;
    startTree(*manager, collector, tree);
    manager.reset();
    if (HasFatalFailure()) {
        return;
    }
    const auto all = tree.all();
    expectAllEnd(tree);
    EXPECT_TRUE(survivors(tree.root, all).empty());
}

} // namespace
} // namespace frost::run
