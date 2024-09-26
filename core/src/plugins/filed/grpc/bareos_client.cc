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

#include "bareos_client.h"

bool BareosClient::Register(std::basic_string_view<bc::EventType> types)
{
  bc::RegisterRequest req;

  for (auto type : types) { req.add_event_types(type); }


  bc::RegisterResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Events_Register(&ctx, req, &resp);

  if (!status.ok()) { return false; }

  return true;
}
bool BareosClient::Unregister(std::basic_string_view<bc::EventType> types)
{
  bc::UnregisterRequest req;

  for (auto type : types) { req.add_event_types(type); }


  bc::UnregisterResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Events_Unregister(&ctx, req, &resp);

  if (!status.ok()) { return false; }

  return true;
}

// TODO: implement these

void BareosClient::AddExclude() { return; }
void BareosClient::AddInclude() { return; }
void BareosClient::AddOptions() { return; }
void BareosClient::AddRegex() { return; }
void BareosClient::AddWild() { return; }
void NewOptions() { return; }
void NewInclude() { return; }
void NewPreInclude() { return; }

//

std::optional<size_t> BareosClient::getInstanceCount()
{
  bc::getInstanceCountRequest req;
  bc::getInstanceCountResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Bareos_getInstanceCount(&ctx, req, &resp);

  if (!status.ok()) { return std::nullopt; }

  return resp.instance_count();
}

std::optional<bool> BareosClient::checkChanges(
    bc::FileType ft,
    std::string_view name,
    std::optional<std::string_view> link_name,
    time_t timestamp,
    const struct stat& statp)
{
  bc::checkChangesRequest req;
  req.set_type(ft);
  req.set_file(name.data(), name.size());
  if (link_name) { req.set_link_target(link_name->data(), link_name->size()); }
  req.mutable_since_time()->set_seconds(timestamp);
  req.set_stats(&statp, sizeof(statp));

  bc::checkChangesResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Bareos_checkChanges(&ctx, req, &resp);

  if (!status.ok()) { return std::nullopt; }

  return resp.old();
}
std::optional<bool> BareosClient::AcceptFile(std::string_view name,
                                             const struct stat& statp)
{
  bc::AcceptFileRequest req;
  req.set_file(name.data(), name.size());
  req.set_stats(&statp, sizeof(statp));

  bc::AcceptFileResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Bareos_AcceptFile(&ctx, req, &resp);

  if (!status.ok()) { return std::nullopt; }

  return resp.skip();
}

bool BareosClient::SetSeen(std::optional<std::string_view> name)
{
  bc::SetSeenRequest req;
  if (name) { req.set_file(name->data(), name->size()); }

  bc::SetSeenResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Bareos_SetSeen(&ctx, req, &resp);

  if (!status.ok()) { return false; }

  return true;
}
bool BareosClient::ClearSeen(std::optional<std::string_view> name)
{
  bc::ClearSeenRequest req;
  if (name) { req.set_file(name->data(), name->size()); }

  bc::ClearSeenResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Bareos_ClearSeen(&ctx, req, &resp);

  if (!status.ok()) { return false; }

  return true;
}
void BareosClient::JobMessage(bc::JMsgType type,
                              int line,
                              const char* file,
                              const char* fun,
                              std::string_view msg)
{
  bc::JobMessageRequest req;
  req.set_type(type);
  req.set_msg(msg.data(), msg.size());
  req.set_line(line);
  req.set_file(file);
  req.set_function(fun);

  bc::JobMessageResponse resp;
  grpc::ClientContext ctx;
  (void)stub_->Bareos_JobMessage(&ctx, req, &resp);
}

void BareosClient::DebugMessage(int level,
                                std::string_view msg,
                                int line,
                                const char* file,
                                const char* fun)
{
  bc::DebugMessageRequest req;
  req.set_level(level);
  req.set_msg(msg.data(), msg.size());
  req.set_line(line);
  req.set_file(file);
  req.set_function(fun);

  bc::DebugMessageResponse resp;
  grpc::ClientContext ctx;
  (void)stub_->Bareos_DebugMessage(&ctx, req, &resp);
}
