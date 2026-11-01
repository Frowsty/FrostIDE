// SPDX-License-Identifier: Apache-2.0
//
// Implementation of SystemSessionState (see frost/perf/win/system_session_state.hpp).

#include "frost/perf/win/system_session_state.hpp"

#include "utf8.hpp"
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>

#include <windows.h>

#include <dxgi1_6.h>
#include <powrprof.h>
#include <wtsapi32.h>

namespace frost::perf::win {

using Microsoft::WRL::ComPtr;

namespace {

// GUID_CONSOLE_DISPLAY_STATE (6FE69556-704A-47A0-8F24-C28D936FDA47), spelled out so this
// file needs no INITGUID translation unit: 0 off, 1 on, 2 dimmed.
constexpr GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

void readInputDesktop(SessionFacts& facts) {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) {
        return;
    }
    facts.inputDesktopOpened = true;
    std::array<wchar_t, 256> name{};
    DWORD needed = 0;
    if (GetUserObjectInformationW(desktop, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed) != FALSE) {
        facts.inputDesktopName = detail::toUtf8(name.data());
    }
    CloseDesktop(desktop);
}

void readLockState(SessionFacts& facts) {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buffer,
                                    &bytes) == FALSE) {
        return;
    }
    if (bytes >= sizeof(WTSINFOEXW)) {
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (info->Level == 1) {
            // Windows 7 had these two values swapped; Windows 8 and later report them as documented.
            switch (info->Data.WTSInfoExLevel1.SessionFlags) {
            case WTS_SESSIONSTATE_LOCK: facts.lock = LockState::Locked; break;
            case WTS_SESSIONSTATE_UNLOCK: facts.lock = LockState::Unlocked; break;
            default: break;
            }
        }
    }
    WTSFreeMemory(buffer);
}

void readSessions(SessionFacts& facts) {
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE) {
        facts.processSession = session;
    }
    facts.activeConsoleSession = WTSGetActiveConsoleSessionId();
}

// Shared with the power-setting callback, which runs on a system thread.
struct DisplayStateWait {
    HANDLE ready = nullptr;
    std::atomic<int> state{-1};
};

ULONG CALLBACK onDisplayState(PVOID context, ULONG type, PVOID setting) {
    auto* wait = static_cast<DisplayStateWait*>(context);
    if (type == PBT_POWERSETTINGCHANGE && setting != nullptr) {
        const auto* broadcast = static_cast<const POWERBROADCAST_SETTING*>(setting);
        if (broadcast->DataLength >= sizeof(DWORD)) {
            DWORD value = 0;
            std::memcpy(&value, broadcast->Data, sizeof(value));
            wait->state.store(static_cast<int>(value), std::memory_order_release);
            SetEvent(wait->ready);
        }
    }
    return ERROR_SUCCESS;
}

void readDisplayPower(SessionFacts& facts) {
    /* The callback context lives on the heap. PowerSettingUnregisterNotification
       is not documented to wait for a callback already in flight, so after a
       timeout the state (and its event) is deliberately leaked: a late callback
       then writes to live memory. A few bytes per timed-out precheck, which only
       happens when the power manager does not answer within 500 ms. */
    auto* wait = new DisplayStateWait;
    wait->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (wait->ready == nullptr) {
        delete wait;
        return;
    }
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters{};
    parameters.Callback = &onDisplayState;
    parameters.Context = wait;
    HPOWERNOTIFY registration = nullptr;
    bool answered = false;
    if (PowerSettingRegisterNotification(&kConsoleDisplayState, DEVICE_NOTIFY_CALLBACK, &parameters, &registration) ==
        ERROR_SUCCESS) {
        answered = WaitForSingleObject(wait->ready, 500) == WAIT_OBJECT_0;
        PowerSettingUnregisterNotification(registration);
    }
    const int state = wait->state.load(std::memory_order_acquire);
    if (answered || registration == nullptr) {
        // The one callback has run (or none was registered), so nothing can touch `wait` again.
        CloseHandle(wait->ready);
        delete wait;
    }
    switch (state) {
    case 0: facts.consoleDisplay = DisplayPower::Off; break;
    case 1: facts.consoleDisplay = DisplayPower::On; break;
    case 2: facts.consoleDisplay = DisplayPower::Dimmed; break;
    default: break;
    }
}

Result<void> readOutputs(SessionFacts& facts) {
    ComPtr<IDXGIFactory1> factory;
    const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return fail(ErrorCode::IoError,
                    std::format("CreateDXGIFactory1 failed: HRESULT 0x{:08X}", static_cast<unsigned>(hr)));
    }
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        // NOT_FOUND ends the list; any other failure (a device lost mid-enumeration) leaves a null pointer.
        if (FAILED(factory->EnumAdapters1(a, &adapter))) {
            break;
        }
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(o, &output))) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) {
                continue;
            }
            OutputFacts out;
            out.deviceName = detail::toUtf8(desc.DeviceName);
            out.attachedToDesktop = desc.AttachedToDesktop != FALSE;
            out.left = desc.DesktopCoordinates.left;
            out.top = desc.DesktopCoordinates.top;
            out.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            out.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE) {
                out.refreshHz = static_cast<double>(mode.dmDisplayFrequency);
            }
            facts.outputs.push_back(std::move(out));
        }
    }
    return {};
}

} // namespace

Result<SessionFacts> SystemSessionState::facts() const {
    SessionFacts facts;
    readInputDesktop(facts);
    readLockState(facts);
    readSessions(facts);
    readDisplayPower(facts);
    if (auto outputs = readOutputs(facts); !outputs) {
        return std::unexpected(std::move(outputs).error());
    }
    return facts;
}

} // namespace frost::perf::win
