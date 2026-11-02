// SPDX-License-Identifier: Apache-2.0
//
// DocumentFixture: the shared setup of the frost::document unit tests: a MemFs over a ManualClock
// with a work directory, file helpers, and a lazily built SaveManager whose options and services a
// test may adjust first. Header-only; test code only.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/text/document.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace frost::document::test {

class DocumentFixture : public ::testing::Test {
protected:
    // POSIX paths on every platform (as the settings tests): the manager's logic is the same.
    DocumentFixture() : fs(clock, io::PathStyle::Posix) {
        options.style = io::PathStyle::Posix;
        EXPECT_TRUE(fs.createDirectories(workDirectory()));
    }

    static std::string workDirectory() { return "/work"; }

    static std::string path(std::string_view name) { return std::string("/work/").append(name); }

    // Writes `bytes` as another program would, one tick later so the modification time moves.
    void writeExternally(std::string_view name, std::string_view bytes) {
        clock.advance(std::chrono::milliseconds(10));
        ASSERT_TRUE(fs.writeFileAtomic(path(name), bytes));
    }

    [[nodiscard]] std::string read(std::string_view name) {
        auto bytes = fs.readFile(path(name));
        return bytes ? std::move(*bytes) : std::string("<missing>");
    }

    // The manager, built at the first call from `options`, `policies` and `services`.
    SaveManager& manager() {
        if (!m_manager) {
            services.fs = &fs;
            services.clock = &clock;
            m_manager = std::make_unique<SaveManager>(services, options, policies);
        }
        return *m_manager;
    }

    void resetManager() { m_manager.reset(); }

    // Opens `name`; a test failure and nullptr on error.
    FileDocument* open(std::string_view name) {
        auto opened = manager().open(path(name));
        if (!opened) {
            ADD_FAILURE() << opened.error().describe();
            return nullptr;
        }
        return *opened;
    }

    // Replaces bytes [begin, end) of the document's text with `text`, as the editor would.
    static void edit(FileDocument& document, std::uint64_t begin, std::uint64_t end, std::string_view text) {
        auto changed =
            document.document().replace(text::ByteRange{ByteOffset{begin}, ByteOffset{end}}, text, text::EditContext{});
        ASSERT_TRUE(changed) << changed.error().describe();
        document.document().breakCoalescing();
    }

    static std::string textOf(const FileDocument& document) { return document.document().committedText().text(); }

    // Whether any writeFileAtomic / safeWrite temp file is left in the MemFs.
    [[nodiscard]] bool tempFilesLeft() const {
        for (const std::string& entry : fs.allPaths()) {
            if (entry.find(".frost-tmp-") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    ManualClock clock{IClock::SteadyTime{std::chrono::hours(1)}, IClock::SystemTime{std::chrono::hours(24 * 365 * 50)}};
    io::MemFs fs;
    SaveManagerServices services{};
    SaveManagerOptions options{};
    SavePolicies policies{};

private:
    std::unique_ptr<SaveManager> m_manager;
};

} // namespace frost::document::test
