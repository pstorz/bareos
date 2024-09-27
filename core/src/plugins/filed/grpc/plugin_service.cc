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
#include <fcntl.h>
#include "bareos.pb.h"
#include "common.pb.h"
#include "plugin.pb.h"
#include "test_module.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"

auto PluginService::Setup(ServerContext*,
                          const bp::SetupRequest*,
                          bp::SetupResponse*) -> Status
{
  auto events = std::array{
      bc::EventType::Event_JobStart, bc::EventType::Event_JobEnd,
      bc::EventType::Event_BackupCommand, bc::EventType::Event_StartBackupJob,
      bc::EventType::Event_EndBackupJob};

  if (!Register({events.data(), events.size()})) {
    DebugLog(50, FMT_STRING("could not register events!"));
  } else {
    DebugLog(100, FMT_STRING("managed to register my events!"));
  }
  return Status::OK;
}

auto PluginService::handlePluginEvent(
    ServerContext*,
    const bp::handlePluginEventRequest* request,
    bp::handlePluginEventResponse* response) -> Status
{
  auto& event = request->to_handle();

  DebugLog(100, FMT_STRING("got some event"));

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
    response->set_res(bp::ReturnCode::RC_OK);
  } else if (event.has_cancel_command()) {
    auto& inner = event.cancel_command();
  } else if (event.has_end_backup_job()) {
    auto& inner = event.end_backup_job();
    DebugLog(100, FMT_STRING("got backup end event ({})."),
             inner.DebugString());
    response->set_res(bp::RC_OK);
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
    DebugLog(100, FMT_STRING("got backup end event ({})."),
             inner.DebugString());

    std::optional path = Bareos_GetString(bc::BV_ExePath);

    if (path) { files_to_backup.emplace_back(std::move(*path)); }

    if (files_to_backup.empty()) {
      response->set_res(bp::RC_Stop);
    } else {
      response->set_res(bp::RC_OK);
    }
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

  if (response->res() == bp::RETURN_CODE_UNSPECIFIED) {
    return Status(grpc::StatusCode::UNIMPLEMENTED,
                  "i lied about handling this particular event");
  }

  return Status::OK;
}
auto PluginService::startBackupFile(ServerContext*,
                                    const bp::startBackupFileRequest* request,
                                    bp::startBackupFileResponse* response)
    -> Status
{
  if (files_to_backup.size() == 0) {
    DebugLog(100, FMT_STRING("no more files left; we are done"));
    response->set_result(bp::StartBackupFileResult::SBF_Stop);
    return grpc::Status::OK;
  }

  auto& file = files_to_backup.back();
  DebugLog(100, FMT_STRING("starting backup of file {}"),
           std::string_view{file});

  struct stat statp;
  if (stat(file.c_str(), &statp) < 0) {
    DebugLog(100, FMT_STRING("could not stat {}"), file);
    response->set_result(bp::StartBackupFileResult::SBF_Skip);
    return grpc::Status::OK;
  }

  auto* f = response->mutable_file();
  f->set_file(std::move(file));
  f->set_stats(&statp, sizeof(statp));
  f->set_delta_seq(0);
  if (S_ISDIR(statp.st_mode)) {
    f->set_ft(bco::FT_DIREND);
    f->set_no_read(true);
  } else {
    f->set_ft(bco::FT_REG);
    f->set_no_read(false);
  }
  f->set_portable(true);  // default value

  response->set_result(bp::SBF_OK);

  return Status::OK;
}
auto PluginService::endBackupFile(ServerContext*,
                                  const bp::endBackupFileRequest* request,
                                  bp::endBackupFileResponse* response) -> Status
{
  if (current_file) { current_file.reset(); }
  return Status::OK;
  // im not sure if this is really the case!
  //  else {
  //   return Status(grpc::StatusCode::FAILED_PRECONDITION,
  //                 "No file is currently open!");
  // }
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
                             bp::fileOpenResponse*) -> Status
{
  if (current_file) {
    DebugLog(100, FMT_STRING("trying to open {} while fd {} is still open"),
             request->file(), current_file->get());
    return Status(grpc::StatusCode::FAILED_PRECONDITION,
                  "there is still a file open");
  }

  int fd = open(request->file().c_str(), request->flags(), request->mode());

  DebugLog(100, FMT_STRING("open(file = {}, flags = {}, mode = {}) -> {}"),
           request->file(), request->flags(), request->mode(), fd);

  if (fd < 0) {
    return Status(grpc::StatusCode::INVALID_ARGUMENT,
                  "could not open specified file given flags/mode.");
  }

  current_file = raii_fd(fd);

  return Status::OK;
}
auto PluginService::FileSeek(ServerContext*,
                             const bp::fileSeekRequest* request,
                             bp::fileSeekResponse*) -> Status
{
  if (!current_file) {
    DebugLog(100, FMT_STRING("trying to seek file while it is not open"));
    return Status(grpc::StatusCode::FAILED_PRECONDITION,
                  "there is no open file");
  }

  auto offset = request->offset();
  int whence = 0;

  switch (request->whence()) {
    case bp::SS_StartOfFile: {
      whence = SEEK_SET;
    } break;
    case bp::SS_CurrentPos: {
      whence = SEEK_CUR;
    } break;
    case bp::SS_EndOfFile: {
      whence = SEEK_END;
    } break;
    default:
      return Status(grpc::StatusCode::INVALID_ARGUMENT,
                    "invalid start position for seek");
  }
  auto res = lseek(current_file->get(), offset, whence);
  DebugLog(100,
           FMT_STRING("lseek(fd = {}, offset = {}, whence = {} ({})) -> {}"),
           current_file->get(), offset, whence,
           bp::SeekStart_Name(request->whence()), res);
  if (res < 0) {
    return Status(grpc::StatusCode::UNKNOWN,
                  fmt::format(FMT_STRING("lseek returned error {}: Err={}"),
                              res, strerror(errno)));
  }

  return Status::OK;
}
auto PluginService::FileRead(ServerContext*,
                             const bp::fileReadRequest* request,
                             bp::fileReadResponse* response) -> Status
{
  if (!current_file) {
    DebugLog(100, FMT_STRING("trying to read file while it is not open"));
    return Status(grpc::StatusCode::FAILED_PRECONDITION,
                  "there is no open file");
  }

  auto max_size = request->num_bytes();

  std::string buffer;
  buffer.resize(max_size);
  auto res = read(current_file->get(), buffer.data(), buffer.size());
  DebugLog(100, FMT_STRING("read(fd = {}, buffer = {}, count = {}) -> {}"),
           current_file->get(), (void*)buffer.data(), buffer.size(), res);

  if (res < 0) {
    return Status(grpc::StatusCode::UNKNOWN,
                  fmt::format(FMT_STRING("read returned error {}: Err={}"), res,
                              strerror(errno)));
  }

  response->set_content(std::move(buffer));
  return Status::OK;
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
  if (!current_file) {
    DebugLog(100, FMT_STRING("trying to close file while it is not open"));
    return Status(grpc::StatusCode::FAILED_PRECONDITION,
                  "there is no open file");
  }

  current_file.reset();
  return Status::OK;
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
