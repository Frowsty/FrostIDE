// SPDX-License-Identifier: Apache-2.0
/*
 * RUN-1 acceptance (label integration): Stop leaves no descendant process. run_tree_child builds a
 * real tree of seven processes (the program, two children, four grandchildren; on Linux one branch
 * in a process group of its own); the test checks with the OS process table (process_table.hpp)
 * that the tree is what the children reported, stops the run through RunManager, and checks by
 * enumeration again that none of them is left. Also: destroying the manager ends a running tree.
 *
 * Safe under parallel load on a shared machine: on Windows only parent links proven by creation
 * times count (process_table.hpp, verifiedLink); every process the test may kill is first verified
 * to be the tree's; and handles to all of them stay open until the final enumeration, so none of
 * their pids can name another process before the test has looked.
 */

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "io/process_env.hpp"
#include "run/process_table.hpp"
#include "run/run_collector.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
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

// Whether `pid`, to which the caller holds a handle, is the process a table listed with creation time `created`.
bool sameProcess([[maybe_unused]] io::ProcessId pid, [[maybe_unused]] std::uint64_t created) {
#ifdef _WIN32
    return created != 0 && test::creationTime(pid) == created;
#else
    return false; // Only used for console hosts, which Linux does not have.
#endif
}

// Starts the tree and waits until all seven processes reported; checks the tree against the OS table.
struct StartedTree {
    RunId id = 0;
    io::ProcessId root = 0;
    std::set<io::ProcessId> pids;  // The reported run_tree_child processes, the root included.
    std::set<io::ProcessId> hosts; // Their console hosts (Windows), verified in the process table.
    /*
     * Open handles to the processes verified to belong to the tree (of pids and hosts). They are the
     * only processes expectAllEnd may kill, and while they are open none of their pids can be reused.
     */
    std::vector<std::unique_ptr<test::ProcessWatch>> watches;

    // Every process of the tree.
    [[nodiscard]] std::set<io::ProcessId> all() const {
        std::set<io::ProcessId> out = pids;
        out.insert(hosts.begin(), hosts.end());
        return out;
    }
};

/*
 * The live processes of the tree by a fresh enumeration: one of tree.all(), or (run_tree_child or a
 * console host) below the root by verified links. Call it while tree.watches are still open, so the
 * pids still name the tree's processes, ended or not.
 */
std::vector<test::OsProcess> survivors(const StartedTree& tree) {
    const auto table = test::processTable();
    const auto below = test::descendantsOf(tree.root, table);
    const auto all = tree.all();
    std::vector<test::OsProcess> left;
    for (const test::OsProcess& process : table) {
        const bool ours = all.contains(process.pid) ||
                          (below.contains(process.pid) && (process.image == treeImage() || consoleHost(process)));
        if (ours && running(process)) {
            left.push_back(process);
        }
    }
    return left;
}

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
    /*
     * Handles opened before the table is read, so each reported pid names the same process in the
     * table as in its handle. A handle is kept only once the table proves its process is the tree's,
     * since a reported process could in principle have ended and its pid been reused by now.
     */
    std::map<io::ProcessId, std::unique_ptr<test::ProcessWatch>> reportedWatches;
    for (const io::ProcessId pid : tree.pids) {
        auto watch = std::make_unique<test::ProcessWatch>(pid);
        ASSERT_TRUE(watch->valid()) << pid;
        reportedWatches.emplace(pid, std::move(watch));
    }

    // The oracle: below the program, the OS process table holds exactly the processes that reported
    // themselves, plus (Windows) their console hosts. Links are followed only where verified.
    const auto table = test::processTable();
    ASSERT_FALSE(table.empty()) << "the process table could not be read";
    std::set<io::ProcessId> expectedBelow = tree.pids;
    expectedBelow.erase(tree.root);
    const auto below = test::descendantsOf(tree.root, table);
    bool rootListed = false;
    std::set<io::ProcessId> belowTree;
    std::map<io::ProcessId, std::uint64_t> hostCreated;
    for (const test::OsProcess& process : table) {
        if (process.pid == tree.root) {
            rootListed = process.image == treeImage();
            continue;
        }
        if (!below.contains(process.pid)) {
            continue;
        }
        if (process.image == treeImage()) {
            belowTree.insert(process.pid);
        } else if (consoleHost(process) && tree.pids.contains(process.parent)) {
            // A console host counts only when a reported process is its (verified) parent.
            hostCreated.emplace(process.pid, process.created);
        } else {
            ADD_FAILURE() << "an unexpected process below the program: " << process.image << " " << process.pid;
        }
    }
    EXPECT_TRUE(rootListed) << "the program is not in the process table as " << treeImage();
    EXPECT_EQ(belowTree, expectedBelow);
    // Keep the handles of the reported processes the table proved to be the tree's.
    for (auto& [pid, watch] : reportedWatches) {
        if ((pid == tree.root && rootListed) || belowTree.contains(pid)) {
            tree.watches.push_back(std::move(watch));
        }
    }
    for (const auto& [pid, created] : hostCreated) {
        auto watch = std::make_unique<test::ProcessWatch>(pid);
        // A host may have ended with its console since the table was read, and its pid been reused:
        // keep the handle only if it holds the very process the table listed (same creation time).
        if (watch->valid() && sameProcess(pid, created)) {
            tree.hosts.insert(pid);
            tree.watches.push_back(std::move(watch));
        }
    }
}

/*
 * Waits for every watched process (each verified as the tree's) to end; kills and reports any that
 * does not (the test's cleanup). The handles stay open: the caller enumerates survivors() next and
 * then calls releaseWatches.
 */
void expectAllEnd(const StartedTree& tree) {
    for (const auto& watch : tree.watches) {
        if (!watch->exitsWithin(10s)) {
            ADD_FAILURE() << "a process of the tree is still running";
            watch->kill();
        }
    }
}

// Closes the handles, so Windows drops the ended processes from its table.
void releaseWatches(StartedTree& tree) { tree.watches.clear(); }

std::string describe(const std::vector<test::OsProcess>& left) {
    return left.empty() ? std::string()
                        : std::to_string(left.size()) + " process(es), first " + left.front().image + " " +
                              std::to_string(left.front().pid);
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
    expectAllEnd(tree);
    const auto left = survivors(tree);
    releaseWatches(tree);
    EXPECT_TRUE(left.empty()) << describe(left) << " of the tree survived Stop";
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
    expectAllEnd(tree);
    const auto left = survivors(tree);
    releaseWatches(tree);
    EXPECT_TRUE(left.empty()) << describe(left) << " of the tree survived destroying the manager";
}

} // namespace
} // namespace frost::run
