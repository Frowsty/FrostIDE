// SPDX-License-Identifier: Apache-2.0
//
// PrivateDesktop and NoHardErrorBoxes (Windows): keep every box a test child could show
// off the user's screen. PrivateDesktop gives the child a desktop of its own for the
// windows it creates; NoHardErrorBoxes turns off the system's error boxes for it.
//
/*
 * Why: a test child that fails can show a modal box and wait for a click that never
 * comes. On the interactive desktop that box appears in front of whatever the user is
 * doing. There are two kinds, and each needs its own guard:
 *   - Boxes the child draws itself: Qt's "no Qt platform plugin could be initialized"
 *     box in Release builds (which nothing in the child can suppress), the Debug CRT's
 *     assertion box, any MessageBoxW. A window belongs to the desktop of the thread that
 *     creates it, so on a PrivateDesktop these are invisible and the test's own timeout
 *     ends the child. Pass name() as frost::io::ProcessSpec::desktop.
 *   - Hard-error boxes: the loader's "unable to start correctly" (a failed DLL
 *     initialisation, 0xC0000142) and "was not found" (0xC0000135) boxes, and Windows
 *     Error Reporting's crash box. The child raises them through the system (csrss and
 *     WER draw them, not the child), so the child's desktop is not relied on to hold
 *     them back. The error mode the child inherits does that: CreateProcessW passes the
 *     parent's mode on unless CREATE_DEFAULT_ERROR_MODE is given, which frost::io never
 *     passes. A loader failure then just ends the child with its status code.
 *
 * PrivateDesktop: the desktop is created in this process's window station with a random
 * name and the default security descriptor (from this process's token), so children
 * running as the same user can use it. It is never switched to, so it never becomes
 * visible. The destructor closes this process's handle; Windows destroys the desktop once
 * the last thread and handle on it are gone, so a child that is still running keeps it
 * alive. Failure records a test failure (ADD_FAILURE) and leaves valid() false. Callers
 * must then not start the child at all: falling back to the user's desktop is exactly
 * what this class exists to prevent. One instance per child keeps children apart.
 *
 * Header-only; test code only; neither class is thread-safe (see NoHardErrorBoxes).
 */
#pragma once

#ifdef _WIN32

#include "frost/io/utf.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

namespace frost::test {

class PrivateDesktop {
public:
    PrivateDesktop() {
        // std::random_device is the OS RNG in the MSVC STL; the name only has to be unique.
        std::random_device random;
        const std::uint64_t bits = (std::uint64_t{random()} << 32) | random();
        const std::wstring desktop = std::format(L"frost-test-{}-{:016x}", GetCurrentProcessId(), bits);
        m_handle = CreateDesktopW(
            desktop.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
        if (m_handle == nullptr) {
            ADD_FAILURE() << "CreateDesktopW failed (win32 error " << GetLastError()
                          << "); refusing to start a test child on the user's desktop";
            return;
        }
        const std::wstring station = objectName(GetProcessWindowStation());
        auto utf8 = io::fromWide(station + L"\\" + desktop);
        if (station.empty() || !utf8) {
            ADD_FAILURE() << "cannot name this process's window station (win32 error " << GetLastError() << ")";
            CloseDesktop(m_handle);
            m_handle = nullptr;
            return;
        }
        m_name = std::move(*utf8);
    }

    ~PrivateDesktop() {
        if (m_handle != nullptr) {
            CloseDesktop(m_handle);
        }
    }

    PrivateDesktop(const PrivateDesktop&) = delete ("owns a desktop handle");
    PrivateDesktop& operator=(const PrivateDesktop&) = delete ("owns a desktop handle");

    // Whether the desktop exists; false after a failure the constructor reported.
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }

    // "<window station>\<desktop>" in UTF-8, the form ProcessSpec::desktop takes; empty if !valid().
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    // The desktop handle (DESKTOP_CREATEWINDOW, CREATEMENU, READOBJECTS and WRITEOBJECTS), or null.
    [[nodiscard]] HDESK handle() const noexcept { return m_handle; }

    // "<window station>\<desktop>" of the calling thread, in name()'s form; "" on failure.
    [[nodiscard]] static std::string threadDesktopName() {
        const std::wstring station = objectName(GetProcessWindowStation());
        const std::wstring desktop = objectName(GetThreadDesktop(GetCurrentThreadId()));
        if (station.empty() || desktop.empty()) {
            return {};
        }
        return io::fromWide(station + L"\\" + desktop).value_or(std::string());
    }

private:
    // GetUserObjectInformationW(UOI_NAME) of a window station or desktop, or "" on failure.
    static std::wstring objectName(HANDLE object) {
        DWORD needed = 0;
        GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &needed);
        if (needed == 0) {
            return {};
        }
        std::vector<wchar_t> buffer(needed / sizeof(wchar_t) + 1, L'\0');
        if (GetUserObjectInformationW(object, UOI_NAME, buffer.data(),
                                      static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), &needed) == FALSE) {
            return {};
        }
        return buffer.data();
    }

    HDESK m_handle = nullptr;
    std::string m_name;
};

/*
 * While an instance lives, this process's error mode has SEM_FAILCRITICALERRORS,
 * SEM_NOGPFAULTERRORBOX and SEM_NOOPENFILEERRORBOX set, so every child started meanwhile
 * inherits a mode in which the loader and Windows Error Reporting show no box (see the
 * header comment). The destructor restores the previous mode; a running child keeps the
 * mode it inherited. The error mode is process-wide: instances must not overlap on
 * different threads, or a destructor can restore a mode another thread still relies on.
 */
class NoHardErrorBoxes {
public:
    NoHardErrorBoxes() noexcept : m_previous(GetErrorMode()) {
        SetErrorMode(m_previous | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }

    ~NoHardErrorBoxes() { SetErrorMode(m_previous); }

    NoHardErrorBoxes(const NoHardErrorBoxes&) = delete ("restores the error mode once");
    NoHardErrorBoxes& operator=(const NoHardErrorBoxes&) = delete ("restores the error mode once");

private:
    UINT m_previous;
};

} // namespace frost::test

#endif
