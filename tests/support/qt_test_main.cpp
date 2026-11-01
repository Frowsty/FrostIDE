// SPDX-License-Identifier: Apache-2.0
//
// main() for test executables that need Qt (frost_add_test(... QT)).
//
/*
 * It selects the offscreen platform (unless QT_QPA_PLATFORM is already set) and
 * adds Qt's plugin directory from the build configuration, so the tests run
 * headless from ctest without any environment set-up, then creates the single
 * QApplication the tests share and runs GoogleTest.
 */

#include <QApplication>
#include <QCoreApplication>
#include <QString>
#include <gtest/gtest.h>

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QCoreApplication::addLibraryPath(QStringLiteral(FROST_QT_PLUGIN_DIR));
    ::testing::InitGoogleTest(&argc, argv);
    QApplication app(argc, argv);
    return RUN_ALL_TESTS();
}
