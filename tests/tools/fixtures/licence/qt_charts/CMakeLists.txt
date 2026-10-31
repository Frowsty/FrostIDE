# SPDX-License-Identifier: Apache-2.0
# Licence-check negative fixture: Qt Charts is GPL-3.0-only, so this must fail.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts)
target_link_libraries(app PRIVATE Qt6::Widgets Qt6::Charts)
