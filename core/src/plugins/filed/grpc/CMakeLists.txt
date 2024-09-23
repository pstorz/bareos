#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2024-2024 Bareos GmbH & Co. KG
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

include(BareosDisableWarnings)
include(BareosConfigureFile)

add_library(grpc-fd MODULE grpc.cc grpc_impl.cc)
set_target_properties(grpc-fd PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ..)
install(
  TARGETS grpc-fd
  DESTINATION ${plugindir}
  COMPONENT filedaemon
)
target_link_libraries(grpc-fd bareos)

include(DebugEdit)
