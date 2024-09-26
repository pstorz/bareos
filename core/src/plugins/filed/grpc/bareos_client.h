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

#ifndef BAREOS_PLUGINS_FILED_GRPC_BAREOS_CLIENT_H_
#define BAREOS_PLUGINS_FILED_GRPC_BAREOS_CLIENT_H_

#include <optional>
#include <sys/stat.h>

#include "bareos.grpc.pb.h"
#include "bareos.pb.h"

namespace bc = bareos::core;

struct BareosClient {
  BareosClient(std::shared_ptr<grpc::ChannelInterface> channel)
      : stub_(bc::Core::NewStub(channel))
  {
  }

  bool Register(std::basic_string_view<bc::EventType> types);
  bool Unregister(std::basic_string_view<bc::EventType> types);

  // TODO: implement these

  void AddExclude();
  void AddInclude();
  void AddOptions();
  void AddRegex();
  void AddWild();
  void NewOptions();
  void NewInclude();
  void NewPreInclude();

  //

  std::optional<size_t> getInstanceCount();

  std::optional<bool> checkChanges(bc::FileType ft,
                                   std::string_view name,
                                   std::optional<std::string_view> link_name,
                                   time_t timestamp,
                                   const struct stat& statp);

  std::optional<bool> AcceptFile(std::string_view name,
                                 const struct stat& statp);

  bool SetSeen(std::optional<std::string_view> name = std::nullopt);
  bool ClearSeen(std::optional<std::string_view> name = std::nullopt);
  void JobMessage(bc::JMsgType type,
                  int line,
                  const char* file,
                  const char* fun,
                  std::string_view msg);

  void DebugMessage(int level,
                    std::string_view msg,
                    int line,
                    const char* file,
                    const char* fun);

 private:
  std::unique_ptr<bc::Core::Stub> stub_;
};

#endif  // BAREOS_PLUGINS_FILED_GRPC_BAREOS_CLIENT_H_
