// SPDX-License-Identifier: Apache-2.0
//
// StorageChild: storage_test_child started through io::realProcessLauncher() (absolute path,
// contained, stderr merged into stdout), with a reader thread that collects complete output
// lines, waits for a line by prefix, and kill(), which is IProcess::terminate() (kill -9), then
// draining stdout to EOF and waiting for the exit. Test code only.
#pragma once

#include "frost/io/process.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef FROST_STORAGE_TEST_CHILD
#error "FROST_STORAGE_TEST_CHILD must name storage_test_child"
#endif

namespace frost::storage::test {

class StorageChild {
public:
    // Starts the child with `arguments`; a test failure and nullptr if the launch fails.
    static std::unique_ptr<StorageChild> start(std::vector<std::string> arguments) {
        io::ProcessSpec spec;
        spec.executable = FROST_STORAGE_TEST_CHILD;
        spec.arguments = std::move(arguments);
        spec.stdinMode = io::Stdio::Pipe; // The hold modes block on it.
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::MergeIntoStdout;
        auto process = io::realProcessLauncher().launch(spec);
        if (!process) {
            ADD_FAILURE() << "launch storage_test_child: " << process.error().describe();
            return nullptr;
        }
        return std::unique_ptr<StorageChild>(new StorageChild(std::move(*process)));
    }

    StorageChild(const StorageChild&) = delete ("owns the process");
    StorageChild& operator=(const StorageChild&) = delete ("owns the process");
    ~StorageChild() {
        if (!m_finished) {
            static_cast<void>(m_process->terminate());
            finish();
        }
    }

    /*
     * Waits until the `count`-th line (1-based, counted from the start) that starts with `prefix`
     * arrives, and returns it; nullopt (with a test failure) on timeout, on EOF, or when the child
     * reports "error ...".
     */
    std::optional<std::string> waitFor(std::string_view prefix, std::size_t count = 1,
                                       std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
        std::unique_lock lock(m_mutex);
        std::optional<std::string> found;
        const bool done = m_changed.wait_for(lock, timeout, [&] {
            std::size_t seen = 0;
            for (const std::string& text : m_lines) {
                if (text.starts_with("error ")) {
                    return true;
                }
                if (text.starts_with(prefix) && ++seen == count) {
                    found = text;
                    return true;
                }
            }
            return m_eof;
        });
        if (!found) {
            ADD_FAILURE() << "storage_test_child: no line '" << prefix << "' #" << count
                          << (done ? " before EOF or error" : " before the timeout") << "; output:\n"
                          << joinedLocked();
        }
        return found;
    }

    // kill -9, then drain stdout to EOF and wait for the exit (Windows can release locks late).
    void kill() {
        if (auto terminated = m_process->terminate(); !terminated) {
            ADD_FAILURE() << "terminate: " << terminated.error().describe();
        }
        finish();
    }

    // Waits for an exit the child makes on its own; returns its exit code (-1 on timeout).
    int waitForExit(std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
        auto status = m_process->wait(timeout);
        if (!status) {
            ADD_FAILURE() << "storage_test_child did not exit: " << status.error().describe();
            kill();
            return -1;
        }
        finish();
        return status->exitCode;
    }

    // Every complete line so far (all of them once kill() or waitForExit() returned).
    std::vector<std::string> lines() const {
        std::lock_guard lock(m_mutex);
        return m_lines;
    }

    // The number in the last line "<prefix><n>", or -1 if there is none.
    std::int64_t lastNumber(std::string_view prefix) const {
        std::lock_guard lock(m_mutex);
        for (auto it = m_lines.rbegin(); it != m_lines.rend(); ++it) {
            if (it->starts_with(prefix)) {
                return std::stoll(it->substr(prefix.size()));
            }
        }
        return -1;
    }

    std::string output() const {
        std::lock_guard lock(m_mutex);
        return joinedLocked();
    }

private:
    explicit StorageChild(std::unique_ptr<io::IProcess> process) : m_process(std::move(process)) {
        m_reader = std::thread([this] { readLoop(); });
    }

    void readLoop() {
        std::string partial;
        char buffer[4096];
        while (true) {
            auto read = m_process->readStdout(buffer);
            if (!read || *read == 0) {
                break;
            }
            partial.append(buffer, *read);
            std::size_t start = 0;
            std::vector<std::string> complete;
            for (std::size_t newline = partial.find('\n'); newline != std::string::npos;
                 newline = partial.find('\n', start)) {
                std::string text = partial.substr(start, newline - start);
                if (!text.empty() && text.back() == '\r') {
                    text.pop_back();
                }
                complete.push_back(std::move(text));
                start = newline + 1;
            }
            partial.erase(0, start);
            if (!complete.empty()) {
                std::lock_guard lock(m_mutex);
                for (std::string& text : complete) {
                    m_lines.push_back(std::move(text));
                }
                m_changed.notify_all();
            }
        }
        // An incomplete last line (the kill landed mid-print) is dropped: only complete lines count.
        std::lock_guard lock(m_mutex);
        m_eof = true;
        m_changed.notify_all();
    }

    void finish() {
        if (m_reader.joinable()) {
            m_reader.join();
        }
        if (auto status = m_process->wait(std::chrono::seconds(30)); !status) {
            ADD_FAILURE() << "storage_test_child did not exit after kill: " << status.error().describe();
        }
        m_finished = true;
    }

    std::string joinedLocked() const {
        std::string out;
        for (const std::string& text : m_lines) {
            out += text;
            out += '\n';
        }
        return out;
    }

    std::unique_ptr<io::IProcess> m_process;
    std::thread m_reader;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::string> m_lines;
    bool m_eof = false;
    bool m_finished = false;
};

} // namespace frost::storage::test
