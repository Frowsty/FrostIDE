// SPDX-License-Identifier: Apache-2.0
//
// GitClone, the state of one running Clone Repository (FND-9a), shared by the Workbench's clone
// code (workbench_clone.cpp) and its teardown (workbench.cpp). Private to frost::app.

#pragma once

#include <QPointer>
#include <QProgressDialog>

#include <string>
#include <thread>

namespace frost::app {

/*
 * One running `git clone` (Workbench::cloneRepository): the worker thread that owns the child
 * process, and the progress dialog. The worker launches the resolved, absolute git through
 * IProcessLauncher (never PATH), reads its --progress output from stderr and posts it to the GUI
 * thread; a stop request (Cancel, shutdown) terminates the contained process tree, which ends the
 * read. Owned by Workbench::m_clone; the worker is joined before the Workbench goes away, so its
 * queued posts never outlive it.
 */
struct GitClone {
    std::string url;      // As given; passed to git only.
    std::string shownUrl; // The URL with any userinfo redacted, for labels and notifications.
    std::string userInfo; // The URL's userinfo ("user:token"), redacted from git's own lines too.
    std::string directory;
    bool cancelled = false; // GUI thread only.
    QPointer<QProgressDialog> dialog;
    std::jthread worker;
};

} // namespace frost::app
