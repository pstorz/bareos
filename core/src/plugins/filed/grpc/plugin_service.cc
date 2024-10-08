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
#include <grpc/compression.h>
#include "bareos.pb.h"
#include "common.pb.h"
#include "plugin.pb.h"
#include "test_module.h"

#include <sys/sendfile.h>

#include <filesystem>
#include <thread>

#include <sys/socket.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"

auto PluginService::Setup(ServerContext*,
                          const bp::SetupRequest*,
                          bp::SetupResponse*) -> Status
{
  auto events = std::array{
      bc::EventType::Event_JobStart,        bc::EventType::Event_JobEnd,
      bc::EventType::Event_BackupCommand,   bc::EventType::Event_StartBackupJob,
      bc::EventType::Event_EndBackupJob,    bc::EventType::Event_EndRestoreJob,
      bc::EventType::Event_StartRestoreJob, bc::EventType::Event_RestoreCommand,
  };

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
    DebugLog(100, FMT_STRING("got start backup job event ({})."),
             inner.DebugString());

    if (std::optional path = Bareos_GetString(bc::BV_ExePath); path) {
      while (path->size() > 0 && path->back() == '/') { path->pop_back(); }
      *path += "sbin/";
      // BUG: somehow this does not return the right thing!
      DebugLog(100, FMT_STRING("adding exe path {}"), *path);
      files_to_backup.emplace_back(std::move(*path));
    } else {
      DebugLog(100, FMT_STRING("added no exe path"));
    }

    if (std::optional path = Bareos_GetString(bc::BV_PluginPath); path) {
      while (path->size() > 0 && path->back() == '/') { path->pop_back(); }
      DebugLog(100, FMT_STRING("adding plugin path {}"), *path);
      files_to_backup.emplace_back(std::move(*path));
    } else {
      DebugLog(100, FMT_STRING("added no plugin path"));
    }


    if (files_to_backup.empty()) {
      DebugLog(100, FMT_STRING("no files added -> stop"));
      response->set_res(bp::RC_Stop);
    } else {
      DebugLog(100, FMT_STRING("{} files added -> start"),
               files_to_backup.size());
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
  if (stack.size() + files_to_backup.size() == 0) {
    DebugLog(100, FMT_STRING("no more files left; we are done"));
    response->set_result(bp::StartBackupFileResult::SBF_Stop);
    return grpc::Status::OK;
  }

  if (stack.size() == 0) {
    auto new_file = std::move(files_to_backup.back());
    files_to_backup.pop_back();
    stack.emplace_back(std::move(new_file));
  }

  auto file = std::move(stack.back());
  stack.pop_back();

  DebugLog(100, FMT_STRING("starting backup of file {}"), file);

  struct stat statp;
  if (lstat(file.c_str(), &statp) < 0) {
    DebugLog(100, FMT_STRING("could not stat {}"), file);
    response->set_result(bp::StartBackupFileResult::SBF_Skip);
    return grpc::Status::OK;
  }

  auto* f = response->mutable_file();
  f->set_stats(&statp, sizeof(statp));
  f->set_delta_seq(0);
  if (S_ISDIR(statp.st_mode)) {
    f->set_ft(bco::FT_DIREND);
    f->set_no_read(true);

    namespace fs = std::filesystem;

    JobLog(bc::JMSG_INFO, FMT_STRING("directory {}"), file);
    DebugLog(100, FMT_STRING("searching {}"), file);

    if (file.added_children) {
    } else {
      file.added_children = true;
      auto copy = file.name;
      stack.emplace_back(std::move(file));
      for (const auto& entry : fs::directory_iterator(copy)) {
        auto path = entry.path().string();

        JobLog(bc::JMSG_INFO, FMT_STRING("adding {}"), path);
        DebugLog(100, FMT_STRING("adding {}"), path);

        files_to_backup.push_back(path);
      }
    }

  } else if (S_ISLNK(statp.st_mode)) {
    JobLog(bc::JMSG_INFO, FMT_STRING("link {}"), file);
    f->set_ft(bco::FT_LNK);
    f->set_no_read(true);
  } else {
    JobLog(bc::JMSG_INFO, FMT_STRING("file {} (mode = {}, {}, {})"), file,
           statp.st_mode, statp.st_mode & S_IFMT, S_IFLNK);
    f->set_ft(bco::FT_REG);
    f->set_no_read(false);
  }
  f->set_portable(true);  // default value
  f->set_file(std::move(file));

  response->set_result(bp::SBF_OK);

  return Status::OK;
}
auto PluginService::endBackupFile(ServerContext*,
                                  const bp::endBackupFileRequest* request,
                                  bp::endBackupFileResponse* response) -> Status
{
  if (current_file) { current_file.reset(); }
  if (files_to_backup.size() > 0) {
    response->set_result(bp::EndBackupFileResult::EBF_More);
  } else {
    response->set_result(bp::EndBackupFileResult::EBF_Done);
  }
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
  auto command = request->command();
  // we currently dont have any reason to care for the command as we dont have
  // any options that one could set

  JobLog(bareos::core::JMsgType::JMSG_INFO,
         FMT_STRING("got command for restoring file: {}"), command);
  DebugLog(100, FMT_STRING("start restore file {}"), command);

  return Status::OK;
}
auto PluginService::endRestoreFile(ServerContext*,
                                   const bp::endRestoreFileRequest* request,
                                   bp::endRestoreFileResponse* response)
    -> Status
{
  DebugLog(100, FMT_STRING("stop restore file"));
  return Status::OK;
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

// static void full_write(int fd, const char* data, size_t size)
// {
//   size_t bytes_written = 0;
//   while (bytes_written < size) {
//     auto res = write(fd, data + bytes_written, size - bytes_written);
//     DebugLog(100, FMT_STRING("write(fd = {}, buffer = {}, count = {}) ->
//     {}"),
//              fd, (void*)(data + bytes_written), size - bytes_written, res);
//     if (res <= 0) { break; }
//     bytes_written += res;
//   }
// }

// static bool full_read(int fd, char* data, size_t size)
// {
//   size_t bytes_read = 0;
//   while (bytes_read < size) {
//     auto res = read(fd, data + bytes_read, size - bytes_read);
//     DebugLog(100, FMT_STRING("read(fd = {}, buffer = {}, count = {}) -> {}"),
//              fd, (void*)(data + bytes_read), size - bytes_read, res);
//     if (res < 0) { return false; }
//     if (res == 0) { break; }
//     bytes_read += res;
//   }
//   return true;
// }

std::optional<int> receive_fd(int unix_socket, int expected_name)
{
  char name_buf[sizeof(
      expected_name)];  // the "name" is just the plugin fd number
                        // this is used to make sure that we actually got the
                        // correct file descriptor
  int fd;
  int name;
  struct msghdr msg = {};
  char buf[CMSG_SPACE(sizeof(fd))] = {};

  iovec io = {.iov_base = name_buf, .iov_len = sizeof(name_buf)};

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  auto res = recvmsg(unix_socket, &msg, MSG_WAITALL);
  if (res < 0) {
    DebugLog(50, FMT_STRING("recvmsg failed ({}): Err={}"), res,
             strerror(errno));
    return std::nullopt;
  }

  if (res != sizeof(name_buf)) {
    DebugLog(50, FMT_STRING("short message received (len = {})"), res);
    name = -1;
  } else {
    static_assert(sizeof(name) == sizeof(name_buf));
    memcpy(&name, name_buf, sizeof(name));
    DebugLog(100, FMT_STRING("received name = {}"), name);
  }

  if (name != expected_name && expected_name == -1) {
    DebugLog(50, FMT_STRING("names do not match got = {}, expected = {}"), name,
             expected_name);
    return std::nullopt;
  } else {
    DebugLog(100, FMT_STRING("name {} matches expected {}"), name,
             expected_name);
  }

  auto* control = CMSG_FIRSTHDR(&msg);

  if (!control) {
    DebugLog(50, FMT_STRING("no control msg received (len = {})"), res);
    return std::nullopt;
  }

  if (control->cmsg_len != CMSG_LEN(sizeof(fd))) {
    DebugLog(50,
             FMT_STRING("control msg is too small (len = {}, expected = {})"),
             control->cmsg_len, sizeof(fd));
    return std::nullopt;
  }

  DebugLog(100, FMT_STRING("control msg {{type = {}, level = {}}}"),
           control->cmsg_type, control->cmsg_level);
  const unsigned char* data = CMSG_DATA(control);

  // size checked above
  memcpy(&fd, data, sizeof(fd));

  DebugLog(100, FMT_STRING("got fd = {}"), fd);

  return std::make_optional(fd);
}

bool send_fd(int unix_socket, int fd)
{
  struct msghdr msg = {};
  char buf[CMSG_SPACE(sizeof(fd))] = {};
  char name_buf[sizeof(fd)];
  memcpy(name_buf, &fd, sizeof(fd));
  iovec io = {.iov_base = name_buf, .iov_len = sizeof(name_buf)};


  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(fd));

  memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

  msg.msg_controllen = CMSG_SPACE(sizeof(fd));

  if (auto res = sendmsg(unix_socket, &msg, 0); res < 0) {
    DebugLog(50, FMT_STRING("could not send fd {}. Err={}"), fd,
             strerror(errno));
  }

  return false;
}

auto PluginService::FileRead(ServerContext* ctx,
                             const bp::fileReadRequest* request,
                             bp::fileReadResponse* response) -> Status
{
  if (!current_file) {
    DebugLog(50, FMT_STRING("trying to read file while it is not open"));
    return Status(grpc::StatusCode::FAILED_PRECONDITION,
                  "there is no open file");
  }

  auto max_size = request->num_bytes();

  DebugLog(100, FMT_STRING("reading from file {} in {} chunks"),
           current_file->get(), max_size);

  auto res = sendfile(io, current_file->get(), nullptr, max_size);

  if (res < 0) {
    // we need to abort here since we do not know what data was written to the
    // socket
    JobLog(bareos::core::JMsgType::JMSG_FATAL,
           FMT_STRING("Could not send chunk from {} to {}: Err={}"),
           current_file->get(), io, strerror(errno));
    return grpc::Status(grpc::StatusCode::INTERNAL, "Error while reading file");
  }

  response->set_size(res);

  return Status::OK;
}
auto PluginService::FileWrite(ServerContext*,
                              const bp::fileWriteRequest* request,
                              bp::fileWriteResponse* response) -> Status
{
  if (!current_file) {
    DebugLog(50, FMT_STRING("trying to read file while it is not open"));
    return Status(grpc::StatusCode::FAILED_PRECONDITION,
                  "there is no open file");
  }

  auto num_bytes = request->bytes_written();

  auto togo = num_bytes;

  while (togo > 0) {
    auto res = sendfile(current_file->get(), io, nullptr, togo);
    if (res < 0) {
      JobLog(bareos::core::JMsgType::JMSG_FATAL,
             FMT_STRING("Could not send chunk from {} to {}: Err={}"), io,
             current_file->get(), strerror(errno));
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Error while writing file");
    }

    if (res > (ssize_t)togo) {
      JobLog(bareos::core::JMsgType::JMSG_FATAL,
             FMT_STRING(
                 "read {} bytes from {} to {}, but only at most {} expected"),
             res, io, current_file->get(), togo);
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "Error while writing file");
    }
    togo -= res;
  }

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
  auto& pkt = request->pkt();


  JobLog(
      bareos::core::JMsgType::JMSG_INFO,
      FMT_STRING("{{ofname = {}, olname = {}, where = {}, regexwhere = {}}}"),
      pkt.ofname(), pkt.olname(), pkt.where(), pkt.regex_where());


  response->set_status(bareos::plugin::CF_Core);

  return Status::OK;
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
  (void)request;
  response->mutable_content()->set_data("");
  return Status::OK;
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
  return Status::OK;
}
auto PluginService::setXattr(ServerContext*,
                             const bp::setXattrRequest* request,
                             bp::setXattrResponse* response) -> Status
{
  return Status::CANCELLED;
}

#pragma GCC diagnostic pop
