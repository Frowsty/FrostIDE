// SPDX-License-Identifier: Apache-2.0
//
// ISaveAllHandler: the shell's Save All as the build commands use it (W1, ADR 0039). The build side
// only calls it, through BuildController, before every build; the shell implements it over its save
// manager.
#pragma once

#include "frost/core/error.hpp"

#include <functional>

namespace frost::ui {

class ISaveAllHandler {
public:
    // Reports the outcome of one saveAll() call; callable from any thread, once (later calls are ignored).
    using Completion = std::function<void(Result<void> saved)>;

    /*
     * Saves every modified document, then calls `done` once, now or later. A failure (a document
     * that could not be written, or the user cancelled a prompt) keeps the build from starting; the
     * build reports the error. Called on the GUI thread; must not block on user input (show prompts
     * modelessly and call `done` when they close).
     */
    virtual void saveAll(Completion done) = 0;

protected:
    ISaveAllHandler() = default;
    ISaveAllHandler(const ISaveAllHandler&) = default;
    ISaveAllHandler& operator=(const ISaveAllHandler&) = default;
    ~ISaveAllHandler() = default;
};

} // namespace frost::ui
