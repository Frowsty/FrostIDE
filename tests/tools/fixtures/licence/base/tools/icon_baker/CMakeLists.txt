# SPDX-License-Identifier: Apache-2.0
# Licence-check fixture: only the icon baker may use Qt Svg.
find_package(Qt6 REQUIRED COMPONENTS Svg)
target_link_libraries(icon_baker PRIVATE Qt6::Svg)
