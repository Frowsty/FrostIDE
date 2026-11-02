// SPDX-License-Identifier: Apache-2.0
//
// document_test_child: the console helper the document kill test launches through
// io::realProcessLauncher() and kills with IProcess::terminate() (kill -9) in the middle of its saves.
//
//   save-loop PATH   opens PATH through a SaveManager on the real file system and saves
//                    killContent(1), killContent(2), ... over it forever; "saved <n>" after each
//                    save returns
//
// Exit status: 2 for bad arguments, 3 for an error (printed as "error <description>").

#include "frost/core/clock.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/file_system.hpp"

#include "kill_protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void line(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int failed(const frost::Error& error) {
    line("error " + error.describe());
    return 3;
}

int saveLoop(const std::string& path) {
    namespace document = frost::document;
    document::SaveManagerServices services;
    services.fs = &frost::io::realFileSystem();
    services.clock = &frost::systemClock();
    document::SavePolicies policies;
    policies.hotExit = false;
    policies.localHistory = false;
    document::SaveManager manager(services, {}, policies);
    auto opened = manager.open(path);
    if (!opened) {
        return failed(opened.error());
    }
    frost::text::Document& text = (*opened)->document();
    for (std::uint64_t n = 1;; ++n) {
        const std::string content = document::test::killContent(n);
        const frost::text::ByteRange all{frost::ByteOffset{0}, text.committedText().size()};
        if (auto replaced = text.replace(all, content, {}); !replaced) {
            return failed(replaced.error());
        }
        auto saved = manager.save(**opened);
        if (!saved) {
            return failed(saved.error());
        }
        line(std::format("saved {}", n));
    }
}

int run(const std::vector<std::string>& args) {
    if (args.size() == 3 && args[1] == "save-loop") {
        return saveLoop(args[2]);
    }
    line("error usage: document_test_child save-loop PATH");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Never a dialog on the desktop: crashes, aborts and Debug CRT reports go to stderr (merged
    // into stdout by the parent) and end the process.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for ([[maybe_unused]] const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) { // Unused in Release.
        static_cast<void>(_CrtSetReportMode(type, _CRTDBG_MODE_FILE));
        static_cast<void>(_CrtSetReportFile(type, _CRTDBG_FILE_STDERR));
    }
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // argv is UTF-8: the embedded manifest sets the UTF-8 code page on Windows.
    const std::vector<std::string> args(argv, argv + argc);
    const int status = run(args);
    std::fflush(stdout);
    return status;
}
