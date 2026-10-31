# SPDX-License-Identifier: Apache-2.0
# Licence-check fixture: the runtime uses Core, Gui and Widgets only.
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Core Gui Widgets)
# A comment naming Qt6::Charts must not count as a use.
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
target_link_libraries(app PRIVATE
    Qt6::Widgets
    Qt6::GuiPrivate)
