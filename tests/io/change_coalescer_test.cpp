// SPDX-License-Identifier: Apache-2.0
//
/*
 * Tests for ChangeCoalescer (src/io/src/change_coalescer.hpp; ADR 0020): the worked cases of the net
 * rules, and a property test that drives random tree mutations through four event flavours (inotify,
 * ReadDirectoryChangesExW, plain ReadDirectoryChangesW and polling diffs), cuts batches at random
 * points, lets the tree move on before each batch is applied ("lagged re-reads") and checks that the
 * reference consumer (tests/support/watch_mirror.hpp) ends up with exactly the tree on disk, that every
 * Renamed keeps the entry's identity and that no Removed names a path that never existed during the
 * batch. FROST_PROPERTY_SEEDS overrides the seed count per flavour (default 250, so the unit run
 * covers 1k sequences and stays fast; ADR 0031's feature-first phase) and FROST_PROPERTY_SEED_OFFSET
 * the first seed (default 0).
 */

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"

#include "change_coalescer.hpp"
#include "support/test_env.hpp"
#include "support/watch_mirror.hpp"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using frost::io::FileChange;
using frost::io::FileChangeKind;
using frost::io::FileType;
using frost::io::MemFsEvent;
using frost::io::MemFsEventKind;
using frost::io::detail::ChangeCoalescer;
using frost::io::detail::RawEvent;
using frost::io::detail::RawKind;

RawEvent ev(RawKind kind, std::string path, std::optional<FileType> type = FileType::Regular,
            std::uint64_t cookie = 0) {
    return RawEvent{kind, std::move(path), type, false, cookie};
}

FileChange change(FileChangeKind kind, std::string path, std::optional<FileType> type = FileType::Regular,
                  std::string oldPath = {}) {
    return FileChange{kind, std::move(path), std::move(oldPath), type};
}

std::vector<FileChange> runIn(std::string root, char separator, std::initializer_list<RawEvent> events,
                              std::size_t maxPending = 1000) {
    ChangeCoalescer coalescer(std::move(root), separator, maxPending);
    for (const RawEvent& event : events) {
        coalescer.push(event);
    }
    return coalescer.take();
}

std::vector<FileChange> run(std::initializer_list<RawEvent> events, std::size_t maxPending = 1000) {
    return runIn("/w", '/', events, maxPending);
}

TEST(ChangeCoalescerHelpers, BelowIsSeparatorAware) {
    using frost::io::detail::isAtOrBelow;
    using frost::io::detail::isStrictlyBelow;
    EXPECT_TRUE(isStrictlyBelow("/a/b/c", "/a/b", '/'));
    EXPECT_FALSE(isStrictlyBelow("/a/bc", "/a/b", '/'));
    EXPECT_FALSE(isStrictlyBelow("/a/b", "/a/b", '/'));
    EXPECT_TRUE(isAtOrBelow("/a/b", "/a/b", '/'));
    EXPECT_TRUE(isStrictlyBelow("/a", "/", '/'));
    EXPECT_TRUE(isStrictlyBelow("C:\\a", "C:\\", '\\'));
    EXPECT_FALSE(isStrictlyBelow("C:\\a", "C:\\a", '\\'));
}

TEST(ChangeCoalescer, CreatedThenModifiedIsCreated) {
    EXPECT_EQ(run({ev(RawKind::Created, "/w/f"), ev(RawKind::Modified, "/w/f")}),
              (std::vector{change(FileChangeKind::Created, "/w/f")}));
}

TEST(ChangeCoalescer, CreatedThenRemovedIsStillRemoved) {
    // A consumer that applied the previous batch late may have read /w/f (lagged re-reads).
    EXPECT_EQ(run({ev(RawKind::Created, "/w/f"), ev(RawKind::Removed, "/w/f")}),
              (std::vector{change(FileChangeKind::Removed, "/w/f")}));
}

TEST(ChangeCoalescer, RemovedThenCreatedFileIsModified) {
    EXPECT_EQ(run({ev(RawKind::Removed, "/w/f"), ev(RawKind::Created, "/w/f")}),
              (std::vector{change(FileChangeKind::Modified, "/w/f")}));
}

TEST(ChangeCoalescer, DirectoryReplacedByFileIsRescan) {
    EXPECT_EQ(run({ev(RawKind::Removed, "/w/d", FileType::Directory), ev(RawKind::Created, "/w/d")}),
              (std::vector{change(FileChangeKind::Rescan, "/w/d")}));
}

TEST(ChangeCoalescer, RecreatedDirectoryIsRescan) {
    EXPECT_EQ(
        run({ev(RawKind::Removed, "/w/d", FileType::Directory), ev(RawKind::Created, "/w/d", FileType::Directory)}),
        (std::vector{change(FileChangeKind::Rescan, "/w/d", FileType::Directory)}));
}

TEST(ChangeCoalescer, DirectoryMovedOutAndRecreatedIsRescan) {
    // mv d elsewhere; mkdir d: the MovedFrom never pairs.
    EXPECT_EQ(run({ev(RawKind::MovedFrom, "/w/d", FileType::Directory, 3),
                   ev(RawKind::Created, "/w/d", FileType::Directory)}),
              (std::vector{change(FileChangeKind::Rescan, "/w/d", FileType::Directory)}));
}

TEST(ChangeCoalescer, PairedMoveIsRenamed) {
    EXPECT_EQ(
        run({ev(RawKind::MovedFrom, "/w/a", FileType::Regular, 7), ev(RawKind::MovedTo, "/w/b", FileType::Regular, 7)}),
        (std::vector{change(FileChangeKind::Renamed, "/w/b", FileType::Regular, "/w/a")}));
}

TEST(ChangeCoalescer, CaseOnlyRenameOnWindows) {
    EXPECT_EQ(runIn("C:\\w", '\\',
                    {ev(RawKind::MovedFrom, "C:\\w\\a.txt", FileType::Regular, 1),
                     ev(RawKind::MovedTo, "C:\\w\\A.txt", FileType::Regular, 1)}),
              (std::vector{change(FileChangeKind::Renamed, "C:\\w\\A.txt", FileType::Regular, "C:\\w\\a.txt")}));
}

TEST(ChangeCoalescer, NonAsciiAndLongPaths) {
    const std::string deep = "/w/" + std::string(300, 'x') + "/\xC3\xBC";
    EXPECT_EQ(run({ev(RawKind::MovedFrom, "/w/\xE6\x97\xA5\xE6\x9C\xAC", FileType::Regular, 1),
                   ev(RawKind::MovedTo, "/w/\xC3\x9F", FileType::Regular, 1), ev(RawKind::Modified, deep)}),
              (std::vector{
                  change(FileChangeKind::Modified, deep), // Sorted bytewise: 'x' < 0xC3.
                  change(FileChangeKind::Renamed, "/w/\xC3\x9F", FileType::Regular, "/w/\xE6\x97\xA5\xE6\x9C\xAC")}));
}

TEST(ChangeCoalescer, VolumeAndFileSystemRoots) {
    EXPECT_EQ(runIn("/", '/', {ev(RawKind::Created, "/f")}), (std::vector{change(FileChangeKind::Created, "/f")}));
    EXPECT_EQ(runIn("C:\\", '\\', {ev(RawKind::Created, "C:\\f")}),
              (std::vector{change(FileChangeKind::Created, "C:\\f")}));
    EXPECT_TRUE(runIn("C:\\", '\\', {ev(RawKind::Created, "D:\\f")}).empty());
}

TEST(ChangeCoalescer, ChainedMovesKeepTheOrigin) {
    EXPECT_EQ(
        run({ev(RawKind::MovedFrom, "/w/a", FileType::Regular, 1), ev(RawKind::MovedTo, "/w/b", FileType::Regular, 1),
             ev(RawKind::MovedFrom, "/w/b", FileType::Regular, 2), ev(RawKind::MovedTo, "/w/c", FileType::Regular, 2)}),
        (std::vector{change(FileChangeKind::Removed, "/w/b"),
                     change(FileChangeKind::Renamed, "/w/c", FileType::Regular, "/w/a")}));
}

TEST(ChangeCoalescer, MoveAndBackIsModified) {
    const auto out =
        run({ev(RawKind::MovedFrom, "/w/a", FileType::Regular, 1), ev(RawKind::MovedTo, "/w/b", FileType::Regular, 1),
             ev(RawKind::MovedFrom, "/w/b", FileType::Regular, 2), ev(RawKind::MovedTo, "/w/a", FileType::Regular, 2)});
    EXPECT_EQ(out, (std::vector{change(FileChangeKind::Modified, "/w/a"), change(FileChangeKind::Removed, "/w/b")}));
}

TEST(ChangeCoalescer, SwapDegradesToModified) {
    const auto out =
        run({ev(RawKind::MovedFrom, "/w/a", FileType::Regular, 1), ev(RawKind::MovedTo, "/w/t", FileType::Regular, 1),
             ev(RawKind::MovedFrom, "/w/b", FileType::Regular, 2), ev(RawKind::MovedTo, "/w/a", FileType::Regular, 2),
             ev(RawKind::MovedFrom, "/w/t", FileType::Regular, 3), ev(RawKind::MovedTo, "/w/b", FileType::Regular, 3)});
    EXPECT_EQ(out, (std::vector{change(FileChangeKind::Modified, "/w/a"), change(FileChangeKind::Modified, "/w/b"),
                                change(FileChangeKind::Removed, "/w/t")}));
}

TEST(ChangeCoalescer, DirectorySwapIsRescan) {
    const auto dir = FileType::Directory;
    const auto out = run({ev(RawKind::MovedFrom, "/w/a", dir, 1), ev(RawKind::MovedTo, "/w/t", dir, 1),
                          ev(RawKind::MovedFrom, "/w/b", dir, 2), ev(RawKind::MovedTo, "/w/a", dir, 2),
                          ev(RawKind::MovedFrom, "/w/t", dir, 3), ev(RawKind::MovedTo, "/w/b", dir, 3)});
    EXPECT_EQ(out,
              (std::vector{change(FileChangeKind::Rescan, "/w/a", dir), change(FileChangeKind::Rescan, "/w/b", dir),
                           change(FileChangeKind::Removed, "/w/t", dir)}));
}

TEST(ChangeCoalescer, MovedInFromOutsideThenRenamedIsNoRename) {
    // The first MovedTo has no MovedFrom in the watch: its source is unknown, so x -> y cannot be a rename.
    const auto out =
        run({ev(RawKind::MovedTo, "/w/x", FileType::Regular, 9), ev(RawKind::MovedFrom, "/w/x", FileType::Regular, 10),
             ev(RawKind::MovedTo, "/w/y", FileType::Regular, 10)});
    EXPECT_EQ(out, (std::vector{change(FileChangeKind::Removed, "/w/x"), change(FileChangeKind::Modified, "/w/y")}));
}

TEST(ChangeCoalescer, DirectoryMoveCarriesPendingChildren) {
    const auto out = run({ev(RawKind::Modified, "/w/d/f"), ev(RawKind::MovedFrom, "/w/d", FileType::Directory, 4),
                          ev(RawKind::MovedTo, "/w/e", FileType::Directory, 4)});
    EXPECT_EQ(out, (std::vector{change(FileChangeKind::Renamed, "/w/e", FileType::Directory, "/w/d"),
                                change(FileChangeKind::Modified, "/w/e/f")}));
}

TEST(ChangeCoalescer, AtomicSaveIsModified) {
    const auto out = run({ev(RawKind::Created, "/w/.f.frost-tmp-1-1"), ev(RawKind::Modified, "/w/.f.frost-tmp-1-1"),
                          ev(RawKind::MovedFrom, "/w/.f.frost-tmp-1-1", FileType::Regular, 9),
                          ev(RawKind::MovedTo, "/w/f", FileType::Regular, 9)});
    // WatchQueue filters the temp file's own events unless reportFrostTempFiles; here it is Removed.
    EXPECT_EQ(out, (std::vector{change(FileChangeKind::Removed, "/w/.f.frost-tmp-1-1"),
                                change(FileChangeKind::Modified, "/w/f")}));
}

TEST(ChangeCoalescer, UnpairedMovedFromIsRemoved) {
    EXPECT_EQ(run({ev(RawKind::MovedFrom, "/w/a", FileType::Regular, 3), ev(RawKind::Modified, "/w/x")}),
              (std::vector{change(FileChangeKind::Removed, "/w/a"), change(FileChangeKind::Modified, "/w/x")}));
}

TEST(ChangeCoalescer, UnpairedMovedToOfFileIsModifiedOfDirectoryIsRescan) {
    EXPECT_EQ(run({ev(RawKind::MovedTo, "/w/f", FileType::Regular, 5)}),
              (std::vector{change(FileChangeKind::Modified, "/w/f")}));
    EXPECT_EQ(run({ev(RawKind::MovedTo, "/w/d", FileType::Directory, 5)}),
              (std::vector{change(FileChangeKind::Rescan, "/w/d", FileType::Directory)}));
    // inotify: only "not a directory" is known for the moved entry.
    ChangeCoalescer coalescer("/w", '/', 100);
    coalescer.push(RawEvent{RawKind::MovedTo, "/w/f", std::nullopt, true, 5});
    EXPECT_EQ(coalescer.take(), (std::vector{change(FileChangeKind::Modified, "/w/f", std::nullopt)}));
}

TEST(ChangeCoalescer, RemovedDirectorySubsumesChildren) {
    EXPECT_EQ(run({ev(RawKind::Modified, "/w/d/f"), ev(RawKind::Removed, "/w/d/f"),
                   ev(RawKind::Removed, "/w/d", FileType::Directory)}),
              (std::vector{change(FileChangeKind::Removed, "/w/d", FileType::Directory)}));
}

TEST(ChangeCoalescer, OverflowIsRescanAndSubsumes) {
    EXPECT_EQ(
        run({ev(RawKind::Modified, "/w/d/f"), ev(RawKind::Overflow, "/w/d", std::nullopt),
             ev(RawKind::Created, "/w/d/g"), ev(RawKind::Modified, "/w/x")}),
        (std::vector{change(FileChangeKind::Rescan, "/w/d", std::nullopt), change(FileChangeKind::Modified, "/w/x")}));
}

TEST(ChangeCoalescer, TooManyPathsCollapseToRescanRoot) {
    EXPECT_EQ(run({ev(RawKind::Created, "/w/a"), ev(RawKind::Created, "/w/b"), ev(RawKind::Created, "/w/c")}, 2),
              (std::vector{change(FileChangeKind::Rescan, "/w", std::nullopt)}));
}

TEST(ChangeCoalescer, EventsOutsideTheRootAreIgnored) {
    EXPECT_TRUE(run({ev(RawKind::Created, "/other/f"), ev(RawKind::Created, "/wx")}).empty());
}

TEST(ChangeCoalescer, RenameIntoRemovedDirectoryDegrades) {
    // /w/a moves into /w/d, and /w/d is then removed: no Renamed may point below a Removed.
    const auto out =
        run({ev(RawKind::MovedFrom, "/w/a", FileType::Regular, 1), ev(RawKind::MovedTo, "/w/d/a", FileType::Regular, 1),
             ev(RawKind::Removed, "/w/d/a"), ev(RawKind::Removed, "/w/d", FileType::Directory)});
    EXPECT_EQ(out, (std::vector{change(FileChangeKind::Removed, "/w/a"),
                                change(FileChangeKind::Removed, "/w/d", FileType::Directory)}));
}

// --- Property test ---

/*
 * Event flavours, as each backend reports MemFs's mutations (MemFs emits them inotify-style):
 *   Inotify       exact types for directories and creations, "not a directory" otherwise; moves keep their
 *                 cookies across directories; a rename may be split across two reads (its MovedTo comes
 *                 with the next feed, after a batch cut perhaps);
 *   RdcwExtended  exact types; a move between directories arrives as Removed + Created; a rename-over's
 *                 replaced target may be reported Removed first; parents get Modified noise;
 *   RdcwPlain     as RdcwExtended without types, except a rename destination, whose type is looked up
 *                 when the events are read;
 *   Polling       snapshot diffs at random scan points, with Modified for every directory whose list of
 *                 children changed since the previous scan (the directory mtime the real scanner reads).
 */
enum class Flavour : std::uint8_t { Inotify, RdcwExtended, RdcwPlain, Polling };

// A small deterministic PRNG (SplitMix64; public domain, Vigna 2015), so runs match on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }

private:
    std::uint64_t m_state;
};

// Records MemFs events in order; the property converts them to a flavour when it feeds them.
class Recorder final : public frost::io::IMemFsObserver {
public:
    void onMemFsEvents(std::span<const MemFsEvent> events) noexcept override {
        m_events.insert(m_events.end(), events.begin(), events.end());
    }
    std::vector<MemFsEvent>& events() noexcept { return m_events; }

private:
    std::vector<MemFsEvent> m_events;
};

[[nodiscard]] RawKind rawKind(MemFsEventKind kind) {
    switch (kind) {
    case MemFsEventKind::Created: return RawKind::Created;
    case MemFsEventKind::Modified: return RawKind::Modified;
    case MemFsEventKind::Removed: return RawKind::Removed;
    case MemFsEventKind::MovedFrom: return RawKind::MovedFrom;
    case MemFsEventKind::MovedTo: return RawKind::MovedTo;
    }
    return RawKind::Modified;
}

struct Property {
    frost::ManualClock clock;
    frost::io::MemFs fs;
    std::string root;
    char separator;
    Flavour flavour;
    Rng rng;
    Recorder recorder;
    ChangeCoalescer coalescer;
    frost::test::WatchMirror mirror;
    frost::test::MirrorTree polled; // Polling: the last scan.
    std::set<std::string> touched;  // Polling: directories whose children changed since the last scan.
    // The identity model, advanced in the order the coalescer is fed: path -> entry id.
    std::map<std::string, std::uint64_t> ids;
    std::map<std::string, std::uint64_t> startIds; // `ids` when the current batch began.
    std::set<std::string> seen;                    // Paths that existed at some point during the current batch.
    std::map<std::uint64_t, std::vector<std::pair<std::string, std::uint64_t>>> moving; // Cookie -> lifted subtree.
    std::uint64_t nextId = 0;
    std::string trace; // Events and batches, printed when the seed fails.

    static std::string describe(const RawEvent& event) {
        static constexpr std::array<const char*, 6> kinds{"Created",   "Modified", "Removed",
                                                          "MovedFrom", "MovedTo",  "Overflow"};
        std::string text = std::string("  raw ") + kinds[static_cast<std::size_t>(event.kind)] + " " + event.path;
        if (event.type) {
            text += *event.type == FileType::Directory ? " (dir)" : " (file)";
        } else if (event.nonDirectory) {
            text += " (non-dir)";
        }
        if (event.cookie != 0) {
            text += " #" + std::to_string(event.cookie);
        }
        return text + "\n";
    }
    void push(const RawEvent& event) {
        trace += describe(event);
        coalescer.push(event);
    }

    Property(std::uint64_t seed, Flavour f, bool windows)
        : fs(clock, windows ? frost::io::PathStyle::Windows : frost::io::PathStyle::Posix),
          root(windows ? "C:\\w" : "/w"), separator(windows ? '\\' : '/'), flavour(f), rng(seed),
          coalescer(root, separator, rng.chance(20) ? 4 + rng.below(12) : 100'000),
          mirror(fs, root, frost::test::MirrorOptions{.separator = separator, .skipFrostTemp = false}) {
        EXPECT_TRUE(fs.createDirectories(root));
        for (int i = 0; i < 6; ++i) {
            mutate(); // The initial tree, before the watch starts.
        }
        mirror.load();
        polled = mirror.snapshot();
        trace += "initial:";
        for (const auto& [path, node] : polled) {
            trace += " " + path + (node.type == FileType::Directory ? "/" : "");
            ids.emplace(path, ++nextId);
        }
        trace += '\n';
        beginBatch();
        fs.addObserver(recorder);
        recorder.events().clear();
    }
    ~Property() { fs.removeObserver(recorder); }
    Property(const Property&) = delete ("refers to its own members");
    Property& operator=(const Property&) = delete ("refers to its own members");

    [[nodiscard]] bool below(const std::string& path, const std::string& prefix) const {
        return path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == separator;
    }
    [[nodiscard]] std::string parentOf(const std::string& path) const { return path.substr(0, path.rfind(separator)); }

    std::string name() {
        static constexpr std::array<const char*, 5> names{"a", "b", "c", "dd", "e.txt"};
        return names[rng.below(names.size())];
    }
    std::string join(const std::string& directory, const std::string& n) const { return directory + separator + n; }

    // Every entry below the root, split by type.
    void entries(std::vector<std::string>& directories, std::vector<std::string>& files) {
        directories = {root};
        files.clear();
        const std::string prefix = root + separator;
        for (const std::string& path : fs.allPaths()) {
            if (path.starts_with(prefix)) {
                (fs.status(path).value().type == FileType::Directory ? directories : files).push_back(path);
            }
        }
    }

    void removeTree(const std::string& path) {
        auto listed = fs.listDirectory(path);
        if (listed) {
            for (const frost::io::DirEntry& entry : *listed) {
                const std::string child = join(path, entry.name);
                if (entry.type == FileType::Directory) {
                    removeTree(child);
                } else {
                    static_cast<void>(fs.removeFile(child));
                }
            }
            static_cast<void>(fs.removeDirectory(path));
        }
    }

    // One random mutation of the tree.
    void mutate() {
        std::vector<std::string> directories;
        std::vector<std::string> files;
        entries(directories, files);
        const std::string directory = directories[rng.below(directories.size())];
        const std::string content = std::to_string(rng.below(4));
        switch (rng.below(8)) {
        case 0:
        case 1: static_cast<void>(fs.writeFileAtomic(join(directory, name()), content)); break;
        case 2: static_cast<void>(fs.writeInPlace(join(directory, name()), content)); break;
        case 3: static_cast<void>(fs.createDirectories(join(directory, name()))); break;
        case 4:
            if (!files.empty()) {
                static_cast<void>(fs.removeFile(files[rng.below(files.size())]));
            }
            break;
        case 5:
            if (directories.size() > 1) {
                removeTree(directories[1 + rng.below(directories.size() - 1)]);
            }
            break;
        default: {
            std::vector<std::string> all = files;
            all.insert(all.end(), directories.begin() + 1, directories.end());
            if (all.empty()) {
                break;
            }
            const std::string from = all[rng.below(all.size())];
            const std::string to = join(directories[rng.below(directories.size())], name());
            const auto target = fs.status(to);
            const std::size_t before = recorder.events().size();
            const bool reportReplaced = flavour != Flavour::Inotify && target && from != to && rng.chance(50);
            if (fs.rename(from, to) && reportReplaced && recorder.events().size() > before) {
                // Some file systems report the replaced target's removal before the rename.
                recorder.events().insert(recorder.events().begin() + static_cast<std::ptrdiff_t>(before),
                                         MemFsEvent{MemFsEventKind::Removed, to, target->type, 0});
            }
            break;
        }
        }
    }

    // --- The identity model ---

    void eraseSubtree(const std::string& path) {
        ids.erase(path);
        for (auto it = ids.upper_bound(path); it != ids.end() && it->first.starts_with(path);) {
            it = below(it->first, path) ? ids.erase(it) : std::next(it);
        }
    }
    void applyIdentity(const MemFsEvent& event) {
        switch (event.kind) {
        case MemFsEventKind::Created:
            ids[event.path] = ++nextId;
            seen.insert(event.path);
            break;
        case MemFsEventKind::Modified: break;
        case MemFsEventKind::Removed: eraseSubtree(event.path); break;
        case MemFsEventKind::MovedFrom: {
            auto& lifted = moving[event.cookie];
            for (auto it = ids.lower_bound(event.path); it != ids.end() && it->first.starts_with(event.path); ++it) {
                if (it->first == event.path || below(it->first, event.path)) {
                    lifted.emplace_back(it->first.substr(event.path.size()), it->second);
                }
            }
            eraseSubtree(event.path);
            break;
        }
        case MemFsEventKind::MovedTo: {
            eraseSubtree(event.path);
            for (const auto& [suffix, id] : moving[event.cookie]) {
                ids[event.path + suffix] = id;
                seen.insert(event.path + suffix);
            }
            moving.erase(event.cookie);
            break;
        }
        }
    }
    void beginBatch() {
        startIds = ids;
        seen.clear();
        if (flavour == Flavour::Polling) {
            for (const auto& [path, node] : polled) {
                seen.insert(path);
            }
        } else {
            for (const auto& [path, id] : ids) {
                seen.insert(path);
            }
        }
    }

    // --- Feeding the coalescer ---

    // RDCW's parent-directory noise: the parent's last-write time changes with its children.
    void noise(const std::string& path, std::vector<RawEvent>& raw) {
        const std::string parent = parentOf(path);
        if (parent != root && rng.chance(50)) {
            raw.push_back(RawEvent{RawKind::Modified, parent,
                                   flavour == Flavour::RdcwPlain ? std::nullopt : std::optional(FileType::Directory),
                                   false, 0});
        }
    }

    [[nodiscard]] std::vector<RawEvent> convert(std::span<const MemFsEvent> events) {
        std::vector<RawEvent> raw;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const MemFsEvent& event = events[i];
            RawEvent r{rawKind(event.kind), event.path, event.type, false, event.cookie};
            if (flavour == Flavour::Inotify) {
                if (event.type != FileType::Directory &&
                    (r.kind == RawKind::Modified || r.kind == RawKind::Removed || r.kind == RawKind::MovedFrom)) {
                    r.type.reset(); // inotify knows only "not a directory" here.
                    r.nonDirectory = true;
                }
                raw.push_back(std::move(r));
                continue;
            }
            const bool plain = flavour == Flavour::RdcwPlain;
            if (event.kind == MemFsEventKind::MovedFrom && i + 1 < events.size() &&
                events[i + 1].kind == MemFsEventKind::MovedTo && events[i + 1].cookie == event.cookie &&
                parentOf(event.path) != parentOf(events[i + 1].path)) {
                // Between directories: Removed + Created, nothing to pair.
                const MemFsEvent& to = events[i + 1];
                raw.push_back(
                    RawEvent{RawKind::Removed, event.path, plain ? std::nullopt : std::optional(event.type), false, 0});
                noise(event.path, raw);
                raw.push_back(
                    RawEvent{RawKind::Created, to.path, plain ? std::nullopt : std::optional(to.type), false, 0});
                noise(to.path, raw);
                ++i;
                continue;
            }
            if (plain) {
                r.type.reset();
                if (r.kind == RawKind::MovedTo) {
                    if (auto status = fs.status(r.path); status) {
                        r.type = status->type; // Looked up when read, as the watcher does.
                    }
                }
            }
            raw.push_back(std::move(r));
            if (event.kind != MemFsEventKind::Modified) {
                noise(event.path, raw);
            }
        }
        return raw;
    }

    // Folds the recorded events (or, when polling, a snapshot diff) into the coalescer.
    void feed(bool everything) {
        std::vector<MemFsEvent>& recorded = recorder.events();
        if (flavour == Flavour::Polling) {
            for (const MemFsEvent& event : recorded) {
                touched.insert(parentOf(event.path)); // Its list of children changed (or its own mtime).
                if (event.type != FileType::Directory) {
                    touched.insert(event.path); // A written or replaced file gets a new mtime.
                }
            }
            recorded.clear();
            if (!everything && !rng.chance(50)) {
                return;
            }
            const frost::test::MirrorTree now = mirror.snapshot();
            for (const RawEvent& event : diff(polled, now)) {
                push(event);
            }
            for (const auto& [path, node] : now) {
                seen.insert(path);
            }
            polled = now;
            touched.clear();
            return;
        }
        std::size_t count = recorded.size();
        if (flavour == Flavour::Inotify && !everything && rng.chance(20)) {
            // A read that ends between the halves of a rename.
            for (std::size_t i = 1; i < recorded.size(); ++i) {
                if (recorded[i].kind == MemFsEventKind::MovedTo) {
                    count = i;
                    break;
                }
            }
        }
        const std::vector<MemFsEvent> batch(recorded.begin(), recorded.begin() + static_cast<std::ptrdiff_t>(count));
        recorded.erase(recorded.begin(), recorded.begin() + static_cast<std::ptrdiff_t>(count));
        for (const RawEvent& event : convert(batch)) {
            push(event);
        }
        for (const MemFsEvent& event : batch) {
            applyIdentity(event);
        }
        if (rng.chance(3)) {
            std::vector<std::string> directories;
            std::vector<std::string> files;
            entries(directories, files);
            push(RawEvent{RawKind::Overflow, directories[rng.below(directories.size())], std::nullopt, false, 0});
        }
    }

    std::vector<RawEvent> diff(const frost::test::MirrorTree& before, const frost::test::MirrorTree& after) const {
        std::vector<RawEvent> events;
        std::string skipOld;
        std::string skipNew;
        const auto under = [&](const std::string& path, const std::string& prefix) {
            return !prefix.empty() && below(path, prefix);
        };
        for (const auto& [path, node] : before) {
            const auto it = after.find(path);
            if (under(path, skipOld)) {
                continue;
            }
            if (it == after.end() || it->second.type != node.type) {
                events.push_back(RawEvent{RawKind::Removed, path, node.type, false, 0});
                skipOld = path;
            } else if ((node.type == FileType::Regular &&
                        (it->second.content != node.content || touched.contains(path))) ||
                       (node.type == FileType::Directory && touched.contains(path))) {
                events.push_back(RawEvent{RawKind::Modified, path, node.type, false, 0});
            }
        }
        for (const auto& [path, node] : after) {
            const auto it = before.find(path);
            if (under(path, skipNew)) {
                continue;
            }
            if (it == before.end() || it->second.type != node.type) {
                events.push_back(RawEvent{RawKind::Created, path, node.type, false, 0});
                skipNew = path;
            }
        }
        if (touched.contains(root)) {
            events.push_back(RawEvent{RawKind::Modified, root, FileType::Directory, false, 0});
        }
        return events;
    }

    // --- Batches ---

    void checkBatch(const std::vector<FileChange>& changes) {
        std::set<std::string> used;
        for (std::size_t i = 0; i < changes.size(); ++i) {
            const FileChange& c = changes[i];
            if (i > 0) {
                ASSERT_LT(changes[i - 1].path, c.path);
            }
            ASSERT_TRUE(used.insert(c.path).second) << c.path;
            if (c.kind == FileChangeKind::Renamed) {
                ASSERT_FALSE(c.oldPath.empty());
                ASSERT_TRUE(used.insert(c.oldPath).second) << c.oldPath;
                // Identity: the entry at the destination now is the one at the source when the batch began.
                ASSERT_NE(flavour, Flavour::Polling);
                ASSERT_TRUE(startIds.contains(c.oldPath)) << c.oldPath;
                ASSERT_TRUE(ids.contains(c.path)) << c.path;
                ASSERT_EQ(startIds.at(c.oldPath), ids.at(c.path)) << c.oldPath << " -> " << c.path;
            }
            if (c.kind == FileChangeKind::Removed) {
                // Never spurious: the path existed at some point during the batch, or is where the consumer
                // places such a path when it applies this batch's renames.
                bool known = seen.contains(c.path);
                for (const FileChange& r : changes) {
                    if (!known && r.kind == FileChangeKind::Renamed && below(c.path, r.path)) {
                        known = seen.contains(r.oldPath + c.path.substr(r.path.size()));
                    }
                }
                ASSERT_TRUE(known) << c.path;
            }
        }
    }

    void traceBatch(const std::vector<FileChange>& changes) {
        trace += "batch:\n";
        for (const FileChange& c : changes) {
            trace += "  " + frost::test::describe(c) + "\n";
        }
    }

    void cut() {
        std::vector<FileChange> changes = coalescer.take();
        traceBatch(changes);
        checkBatch(changes);
        beginBatch();
        // Lagged re-reads: the tree moves on before the consumer applies the batch.
        std::size_t lag = rng.chance(30) ? 1 + rng.below(3) : 0;
        if (rng.chance(5)) {
            lag += 5 + rng.below(6);
        }
        if (lag != 0) {
            trace += "(lag " + std::to_string(lag) + ")\n";
        }
        for (std::size_t i = 0; i < lag; ++i) {
            mutate();
            feed(false);
        }
        if (!changes.empty()) {
            mirror.apply(frost::io::FileChangeBatch{std::move(changes), 1, frost::io::WatchMode::Native});
        }
    }

    void runSteps(int steps) {
        for (int i = 0; i < steps; ++i) {
            mutate();
            feed(false);
            if (rng.chance(25)) {
                cut();
            }
        }
        // Quiet: everything still pending (lag mutations included) goes out in one last batch.
        feed(true);
        std::vector<FileChange> changes = coalescer.take();
        traceBatch(changes);
        checkBatch(changes);
        if (!changes.empty()) {
            mirror.apply(frost::io::FileChangeBatch{std::move(changes), 1, frost::io::WatchMode::Native});
        }
        EXPECT_TRUE(coalescer.empty());
    }
};

// FROST_PROPERTY_SEEDS (default 250 per flavour: 1k sequences in all) seeds from
// FROST_PROPERTY_SEED_OFFSET (default 0); the stress label runs 200k more as shards (tests/io/CMakeLists.txt).
[[nodiscard]] std::uint64_t environmentNumber(const char* name, std::uint64_t fallback) {
    if (const std::string text = frost::test::environment(name); !text.empty()) {
        return std::strtoull(text.c_str(), nullptr, 10);
    }
    return fallback;
}

void runProperty(Flavour flavour) {
    const std::uint64_t first = environmentNumber("FROST_PROPERTY_SEED_OFFSET", 0);
    const std::uint64_t seeds = environmentNumber("FROST_PROPERTY_SEEDS", 250);
    const bool rdcw = flavour == Flavour::RdcwExtended || flavour == Flavour::RdcwPlain;
    for (std::uint64_t seed = first; seed < first + seeds; ++seed) {
        // Windows path style for half of the RDCW seeds and a quarter of the others.
        const bool windows = rdcw ? seed % 2 == 0 : seed % 4 == 3;
        Property property(seed * 4 + static_cast<std::uint64_t>(flavour), flavour, windows);
        property.runSteps(24);
        if (::testing::Test::HasFatalFailure()) {
            FAIL() << "seed " << seed << "\n" << property.trace;
        }
        const frost::test::MirrorTree expected = property.mirror.snapshot();
        ASSERT_EQ(property.mirror.tree(), expected) << "seed " << seed << "\n" << property.trace;
    }
}

TEST(ChangeCoalescerProperty, InotifyEventsConverge) { runProperty(Flavour::Inotify); }
TEST(ChangeCoalescerProperty, ReadDirectoryChangesExEventsConverge) { runProperty(Flavour::RdcwExtended); }
TEST(ChangeCoalescerProperty, PlainReadDirectoryChangesEventsConverge) { runProperty(Flavour::RdcwPlain); }
TEST(ChangeCoalescerProperty, PollingDiffsConverge) { runProperty(Flavour::Polling); }

} // namespace
