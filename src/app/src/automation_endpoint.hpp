// SPDX-License-Identifier: Apache-2.0
//
// The automation endpoint (FND-7a, ADR 0080): a local socket (a named pipe on Windows) through which
// a test drives a running FrostIDE: open a folder, list and pick CMake presets, configure, build,
// read Problems, navigate, read the editor's position. It exists only in Debug test builds
// (FROST_ENABLE_AUTOMATION_ENDPOINT, which defines FROST_AUTOMATION_ENDPOINT for Debug); in every
// other build AutomationEndpoint::available() is false, start() fails with Unsupported, and the
// binary holds none of the endpoint's code or strings (tests/tools/test_automation_release_probe.py
// checks the Release image for the protocol marker).
//
// Protocol (frost-automation-endpoint/1). One JSON object per line, UTF-8, in both directions. A
// request is {"id": <integer>, "method": "<name>", "params": {...}}; the reply is {"id": <same>,
// "result": {...}} or {"id": <same>, "error": {"code": "<frost::ErrorCode name>", "message": "..."}}.
// The first request of a connection must be "hello" with params {"token": "<the session token>"};
// any other first request, or a wrong token, gets an error and the connection is closed. The
// methods are documented in automation_endpoint.cpp (kMethods). Every result is read from the
// workbench's own state (the CMake service, the Problems model, clangd's session, the editor), never
// made up: an unavailable part says so ("available": false) instead.
//
// Security (ADR 0080): the server listens with QLocalServer::UserAccessOption (only this user's
// processes can connect); a 32-byte random token from the system's CSPRNG is written with the
// server name to the connection file the launcher named (FROST_AUTOMATION_CONNECTION), and only a
// client that read that file can authenticate. Nothing listens unless that variable names a file.
//
// Thread safety: GUI thread only (construction, every request and destruction).
#pragma once

#include "frost/core/error.hpp"

#include <memory>
#include <string>

namespace frost::app {

class Workbench;

class AutomationEndpoint {
public:
    // Whether this build has the endpoint (Debug test builds only).
    [[nodiscard]] static bool available() noexcept;

    /*
     * Listens on a new local server and writes the connection file `connectionFile` (absolute; its
     * directory must exist): {"protocol", "server" (the full server name to connect to), "token",
     * "pid"}, replacing it atomically. `workbench` must outlive the endpoint. Errors: Unsupported
     * (not compiled in), InvalidArgument (a relative path), IoError (listen or write failed).
     */
    [[nodiscard]] static Result<std::unique_ptr<AutomationEndpoint>> start(Workbench& workbench,
                                                                           const std::string& connectionFile);

    ~AutomationEndpoint();
    AutomationEndpoint(const AutomationEndpoint&) = delete ("owns the local server");
    AutomationEndpoint& operator=(const AutomationEndpoint&) = delete ("owns the local server");

    // The full server name clients connect to (a pipe name on Windows, a socket path elsewhere).
    [[nodiscard]] const std::string& serverName() const noexcept;
    // The session token (64 lower-case hex digits).
    [[nodiscard]] const std::string& token() const noexcept;

    struct Impl;

private:
    explicit AutomationEndpoint(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace frost::app
