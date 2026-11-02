// SPDX-License-Identifier: Apache-2.0
//
// Which editor renderer draws an EditorView (ADR 0007 decisions 1, 5 and 8; ADR 0011 A1; ADR 0043):
// the editor.renderer setting, the facts about the session that decide its "auto" value, and the
// rule itself. The rule is a pure function so tests can inject an RDP or WARP environment.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {

// The editor.renderer setting: "auto" (QRhi on Windows unless remote or software), "raster" (always
// QPainter) or "rhi" (always try QRhi; a failure still falls back to raster).
enum class EditorRendererMode : std::uint8_t { Auto, Raster, Rhi };

// The backend that actually draws: the raster QPainter one (the golden oracle and the fallback) or
// the QRhiWidget glyph-atlas one.
enum class EditorBackend : std::uint8_t { Raster, Rhi };

// "auto", "raster", "rhi"; nullopt for anything else (the settings schema rejects other values).
[[nodiscard]] std::optional<EditorRendererMode> parseEditorRendererMode(std::string_view text) noexcept;
[[nodiscard]] std::string_view toString(EditorRendererMode mode) noexcept;
[[nodiscard]] std::string_view toString(EditorBackend backend) noexcept;

/*
 * What the auto rule looks at. `rhiAvailable` says whether this build and Qt platform can composite a
 * QRhiWidget with the editor's shaders (Windows on Qt's "windows" platform: Direct3D 11; false on
 * Linux, which keeps raster until a Linux S1 leg exists, and on offscreen or minimal platforms).
 * `softwareAdapter` is known only once a QRhi exists (WARP, Microsoft Basic Render Driver), so the
 * view asks again with it set after the QRhi backend initialised. A plain value.
 */
struct RendererEnvironment {
    bool rhiAvailable = false;
    bool remoteSession = false;
    bool softwareAdapter = false;
    std::string platform; // The Qt platform name, for the reason text.

    friend bool operator==(const RendererEnvironment&, const RendererEnvironment&) = default;
};

// The rule's answer: the backend and a one-line reason for the log ("auto: remote session (RDP)").
struct BackendChoice {
    EditorBackend backend = EditorBackend::Raster;
    std::string reason;

    friend bool operator==(const BackendChoice&, const BackendChoice&) = default;
};

/*
 * The renderer rule (ADR 0043 decision 2):
 *   raster: raster;
 *   rhi:    QRhi when rhiAvailable (even remote or on a software adapter: the user asked), else raster;
 *   auto:   raster when !rhiAvailable, in a remote session or on a software adapter, else QRhi.
 * A QRhi creation failure or a lost device is handled by the view, which falls back to raster.
 * Pure; O(1).
 */
[[nodiscard]] BackendChoice chooseEditorBackend(EditorRendererMode mode, const RendererEnvironment& environment);

/*
 * The environment of this process: rhiAvailable on Windows when QGuiApplication's platform is
 * "windows", remoteSession from GetSystemMetrics(SM_REMOTESESSION); softwareAdapter is false (it
 * needs a QRhi). Precondition: a QGuiApplication exists. GUI thread; O(1).
 */
[[nodiscard]] RendererEnvironment detectRendererEnvironment();

// Whether an adapter is a software rasteriser: Microsoft's WARP / Basic Render Driver (vendor 0x1414,
// device 0x8c) or one the driver reports as a CPU device. O(1).
[[nodiscard]] bool isSoftwareAdapter(std::uint64_t vendorId, std::uint64_t deviceId, bool cpuDevice) noexcept;

/*
 * Test and diagnostics hooks for the renderer choice (EditorServices::renderer):
 *   environment:     replaces detectRendererEnvironment() (inject an RDP or WARP session);
 *   failRhiCreation: the QRhi backend reports a creation failure as soon as it is attached, so the
 *                    fallback path runs without a GPU or a real platform;
 *   softwareAdapter: the QRhi backend reports this software adapter as soon as it is attached, as
 *                    a QRhi created on WARP would (ignored when failRhiCreation is set).
 * Both reports come before anything the real QRhiWidget reports.
 */
struct EditorRendererHooks {
    std::optional<RendererEnvironment> environment;
    bool failRhiCreation = false;
    std::optional<std::string> softwareAdapter;
};

} // namespace frost::ui
