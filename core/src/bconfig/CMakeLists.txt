#
#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

include(BareosWindowsResource)

add_library(bareosbconfig STATIC bconfig.cc)
add_library(Bareos::BConfig ALIAS bareosbconfig)
target_link_libraries(
  bareosbconfig PUBLIC Bareos::Console Bareos::Dir Bareos::FD Bareos::Lib
                       Bareos::LibSD Bareos::Findlib
)

add_executable(bconfig bconfig_tool.cc)
bareos_windows_resource(bconfig bconfigres.rc.in)
target_link_libraries(bconfig PRIVATE Bareos::BConfig CLI11::CLI11)

install(TARGETS bconfig DESTINATION "${sbindir}")
