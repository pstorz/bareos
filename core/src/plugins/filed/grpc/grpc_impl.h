/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_PLUGINS_FILED_GRPC_GRPC_IMPL_H_
#define BAREOS_PLUGINS_FILED_GRPC_GRPC_IMPL_H_


#include <variant>
#include "include/bareos.h"
#include "filed/fd_plugins.h"

struct simple_event {};
struct string_event {
  std::string_view value;
};
struct int_event {
  intptr_t value;
};
struct rop_event {
  filedaemon::restore_object_pkt* value;
};
struct time_event {
  time_t value;
};
struct save_event {
  filedaemon::save_pkt* value;
};

using plugin_event = std::variant<simple_event,
                                  string_event,
                                  int_event,
                                  rop_event,
                                  time_event,
                                  save_event>;

plugin_event make_plugin_event(filedaemon::bEvent* event, void* data);

struct grpc_connection {};

std::optional<grpc_connection> make_connection(std::string_view program_path);

#endif  // BAREOS_PLUGINS_FILED_GRPC_GRPC_IMPL_H_
