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

#include "plugin_service.h"
#include "test_module.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"

auto PluginService::handlePluginEvent(
    ServerContext*,
    const bp::handlePluginEventRequest* request,
    bp::handlePluginEventResponse* response) -> Status
{
  auto& event = request->to_handle();

  if (event.has_level()) {
    auto& inner = event.level();
    DebugLog(100, FMT_STRING("got level event {{level = {}}}"), inner.level());
  } else if (event.has_since()) {
    auto& inner = event.since();
    DebugLog(100, FMT_STRING("got since event {{time = {}}}"),
             inner.since().seconds());
  } else if (event.has_job_end()) {
    auto& inner = event.job_end();
    DebugLog(100, FMT_STRING("got job end event ({}). shutting down ..."),
             inner.DebugString());

    shutdown_plugin();
  } else if (event.has_job_start()) {
    auto& inner = event.job_start();
    DebugLog(100, FMT_STRING("got job start event ({})."), inner.DebugString());
  } else if (event.has_end_fileset()) {
    auto& inner = event.end_fileset();
  } else if (event.has_option_plugin()) {
    auto& inner = event.option_plugin();
  } else if (event.has_backup_command()) {
    auto& inner = event.backup_command();
  } else if (event.has_cancel_command()) {
    auto& inner = event.cancel_command();
  } else if (event.has_end_backup_job()) {
    auto& inner = event.end_backup_job();
  } else if (event.has_end_verify_job()) {
    auto& inner = event.end_verify_job();
  } else if (event.has_plugin_command()) {
    auto& inner = event.plugin_command();
  } else if (event.has_restore_object()) {
    auto& inner = event.restore_object();
  } else if (event.has_end_restore_job()) {
    auto& inner = event.end_restore_job();
  } else if (event.has_restore_command()) {
    auto& inner = event.restore_command();
  } else if (event.has_vss_init_backup()) {
    auto& inner = event.vss_init_backup();
  } else if (event.has_estimate_command()) {
    auto& inner = event.estimate_command();
  } else if (event.has_start_backup_job()) {
    auto& inner = event.start_backup_job();
  } else if (event.has_start_verify_job()) {
    auto& inner = event.start_verify_job();
  } else if (event.has_vss_init_restore()) {
    auto& inner = event.vss_init_restore();
  } else if (event.has_start_restore_job()) {
    auto& inner = event.start_restore_job();
  } else if (event.has_vss_close_restore()) {
    auto& inner = event.vss_close_restore();
  } else if (event.has_handle_backup_file()) {
    auto& inner = event.handle_backup_file();
  } else if (event.has_new_plugin_options()) {
    auto& inner = event.new_plugin_options();
  } else if (event.has_vss_backup_complete()) {
    auto& inner = event.vss_backup_complete();
  } else if (event.has_vss_create_snapshot()) {
    auto& inner = event.vss_create_snapshot();
  } else if (event.has_vss_prepare_snapshot()) {
    auto& inner = event.vss_prepare_snapshot();
  } else if (event.has_vss_set_backup_state()) {
    auto& inner = event.vss_set_backup_state();
  } else if (event.has_vss_prepare_for_backup()) {
    auto& inner = event.vss_prepare_for_backup();
  } else if (event.has_vss_backup_add_components()) {
    auto& inner = event.vss_backup_add_components();
  } else if (event.has_vss_restore_set_components_selected()) {
    auto& inner = event.vss_restore_set_components_selected();
  } else if (event.has_vss_restore_load_companents_metadata()) {
    auto& inner = event.vss_restore_load_companents_metadata();
  } else {
    return Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown event type");
  }

  return Status::CANCELLED;
}
auto PluginService::startBackupFile(ServerContext*,
                                    const bp::startBackupFileRequest* request,
                                    bp::startBackupFileResponse* response)
    -> Status
{
  return Status::CANCELLED;
}
auto PluginService::endBackupFile(ServerContext*,
                                  const bp::endBackupFileRequest* request,
                                  bp::endBackupFileResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::startRestoreFile(ServerContext*,
                                     const bp::startRestoreFileRequest* request,
                                     bp::startRestoreFileResponse* response)
    -> Status
{
  return Status::CANCELLED;
}
auto PluginService::endRestoreFile(ServerContext*,
                                   const bp::endRestoreFileRequest* request,
                                   bp::endRestoreFileResponse* response)
    -> Status
{
  return Status::CANCELLED;
}
auto PluginService::FileOpen(ServerContext*,
                             const bp::fileOpenRequest* request,
                             bp::fileOpenResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::FileSeek(ServerContext*,
                             const bp::fileSeekRequest* request,
                             bp::fileSeekResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::FileRead(ServerContext*,
                             const bp::fileReadRequest* request,
                             bp::fileReadResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::FileWrite(ServerContext*,
                              const bp::fileWriteRequest* request,
                              bp::fileWriteResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::FileClose(ServerContext*,
                              const bp::fileCloseRequest* request,
                              bp::fileCloseResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::createFile(ServerContext*,
                               const bp::createFileRequest* request,
                               bp::createFileResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::setFileAttributes(
    ServerContext*,
    const bp::setFileAttributesRequest* request,
    bp::setFileAttributesResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::checkFile(ServerContext*,
                              const bp::checkFileRequest* request,
                              bp::checkFileResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::getAcl(ServerContext*,
                           const bp::getAclRequest* request,
                           bp::getAclResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::setAcl(ServerContext*,
                           const bp::setAclRequest* request,
                           bp::setAclResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::getXattr(ServerContext*,
                             const bp::getXattrRequest* request,
                             bp::getXattrResponse* response) -> Status
{
  return Status::CANCELLED;
}
auto PluginService::setXattr(ServerContext*,
                             const bp::setXattrRequest* request,
                             bp::setXattrResponse* response) -> Status
{
  return Status::CANCELLED;
}

#pragma GCC diagnostic pop
