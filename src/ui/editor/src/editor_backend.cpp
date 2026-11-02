// SPDX-License-Identifier: Apache-2.0
//
// The editor renderer rule and the session facts it reads (frost/ui/editor/editor_backend.hpp;
// ADR 0043).

#include "frost/ui/editor/editor_backend.hpp"

#include <QGuiApplication>
#include <QString>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::ui {

std::optional<EditorRendererMode> parseEditorRendererMode(std::string_view text) noexcept {
    if (text == "auto") {
        return EditorRendererMode::Auto;
    }
    if (text == "raster") {
        return EditorRendererMode::Raster;
    }
    if (text == "rhi") {
        return EditorRendererMode::Rhi;
    }
    return std::nullopt;
}

std::string_view toString(EditorRendererMode mode) noexcept {
    switch (mode) {
    case EditorRendererMode::Auto: return "auto";
    case EditorRendererMode::Raster: return "raster";
    case EditorRendererMode::Rhi: return "rhi";
    }
    return "?";
}

std::string_view toString(EditorBackend backend) noexcept {
    switch (backend) {
    case EditorBackend::Raster: return "raster";
    case EditorBackend::Rhi: return "rhi";
    }
    return "?";
}

BackendChoice chooseEditorBackend(EditorRendererMode mode, const RendererEnvironment& environment) {
    const std::string platform = environment.platform.empty() ? std::string("this platform") : environment.platform;
    switch (mode) {
    case EditorRendererMode::Raster: return {EditorBackend::Raster, "editor.renderer is raster"};
    case EditorRendererMode::Rhi:
        if (!environment.rhiAvailable) {
            return {EditorBackend::Raster, "editor.renderer is rhi, but " + platform + " has no QRhi editor backend"};
        }
        return {EditorBackend::Rhi, "editor.renderer is rhi"};
    case EditorRendererMode::Auto: break;
    }
    if (!environment.rhiAvailable) {
        return {EditorBackend::Raster, "auto: " + platform + " has no QRhi editor backend (raster)"};
    }
    if (environment.remoteSession) {
        return {EditorBackend::Raster, "auto: remote session (RDP), raster"};
    }
    if (environment.softwareAdapter) {
        return {EditorBackend::Raster, "auto: software adapter (WARP), raster"};
    }
    return {EditorBackend::Rhi, "auto: QRhi (Direct3D 11)"};
}

RendererEnvironment detectRendererEnvironment() {
    RendererEnvironment environment;
    environment.platform = QGuiApplication::platformName().toStdString();
#ifdef _WIN32
    // Offscreen and minimal platforms have no RHI surface to composite a QRhiWidget into (the
    // widget reports renderFailed), so only the real windows platform uses the QRhi backend.
    environment.rhiAvailable = QGuiApplication::platformName() == QStringLiteral("windows");
    environment.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0;
#endif
    return environment;
}

bool isSoftwareAdapter(std::uint64_t vendorId, std::uint64_t deviceId, bool cpuDevice) noexcept {
    // The Microsoft Basic Render Driver (WARP) is vendor 0x1414, device 0x8c.
    constexpr std::uint64_t kMicrosoftVendor = 0x1414;
    constexpr std::uint64_t kBasicRenderDevice = 0x8c;
    return cpuDevice || (vendorId == kMicrosoftVendor && deviceId == kBasicRenderDevice);
}

} // namespace frost::ui
