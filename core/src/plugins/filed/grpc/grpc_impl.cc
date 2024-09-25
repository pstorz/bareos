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

#include "events.pb.h"
#include "plugin.grpc.pb.h"
#include "plugin.pb.h"
#include "bareos.grpc.pb.h"
#include "bareos.pb.h"
#include "filed/fd_plugins.h"
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpc/grpc_posix.h>
#include <grpc/grpc_security.h>

#include "plugins/filed/grpc/grpc_impl.h"
#include <fcntl.h>
#include <thread>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/security/server_credentials.h>
#include <sys/wait.h>

#if 0

#  include <sys/wait.h>
#  include <unistd.h>
#  include <cstring>
#  include <cstdlib>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/mman.h>

std::optional<int> receive_fd(filedaemon::CoreFunctions* funcs,
                              int socket)
{
    struct msghdr read_msg = {};

    alignas(struct cmsghdr) char buffer[CMSG_SPACE(sizeof(int))];

    read_msg.msg_control = buffer;
    read_msg.msg_controllen = sizeof(buffer);

    auto rc = recvmsg(socket, &read_msg, 0);

    debug(funcs, 100, "rc = %lld\n", rc);

    if (rc < 0) {
      return std::nullopt;
    }

    debug(funcs, 100, "clen = %lld\n", read_msg.msg_controllen);

    if (read_msg.msg_controllen <= 0) {
      return std::nullopt;
    }

    auto* hdr = CMSG_FIRSTHDR(&read_msg);
    if (!hdr) {
      return std::nullopt;;
    }

    debug(funcs, 100, "hdr = %p, len = %llu\n", hdr, hdr->cmsg_len);

    if (hdr->cmsg_len != CMSG_LEN(sizeof(int))) {
      return std::nullopt;
    }

    auto* data = CMSG_DATA(hdr);

    int fd{-1};

    memcpy(&fd, data, sizeof(int));

    return fd;
}

bool send_fd(int socket, int fd)
{
    struct msghdr write_msg = {};

    alignas(struct cmsghdr) char buffer[CMSG_SPACE(sizeof(int))];

    struct iovec dummy = { .iov_base = (char*)"", .iov_len = 1 };

    write_msg.msg_control = buffer;
    write_msg.msg_controllen = sizeof(buffer); // set to capacity
    write_msg.msg_iov = &dummy;
    write_msg.msg_iovlen = 1;

    auto* cmsg = CMSG_FIRSTHDR(&write_msg);

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    write_msg.msg_controllen = cmsg->cmsg_len; // set to actual size
    if (sendmsg(socket, &write_msg, 0) < 0) {
      return false;
    }

    return true;
}
#endif

#include "bareos_api.h"

plugin_event make_plugin_event(filedaemon::bEvent* event, void* data)
{
  using namespace filedaemon;
  switch ((bEventType)event->eventType) {
    case bEventJobStart:
      [[fallthrough]];
    case bEventPluginCommand:
      [[fallthrough]];
    case bEventNewPluginOptions:
      [[fallthrough]];
    case bEventBackupCommand:
      [[fallthrough]];
    case bEventRestoreCommand:
      [[fallthrough]];
    case bEventEstimateCommand:
      return string_event{(char*)data};
    case bEventEndFileSet:
      [[fallthrough]];
    case bEventJobEnd:
      [[fallthrough]];
    case bEventCancelCommand:
      [[fallthrough]];
    case bEventStartBackupJob:
      [[fallthrough]];
    case bEventEndBackupJob:
      [[fallthrough]];
    case bEventStartVerifyJob:
      [[fallthrough]];
    case bEventEndVerifyJob:
      [[fallthrough]];
    case bEventStartRestoreJob:
      [[fallthrough]];
    case bEventEndRestoreJob:
      [[fallthrough]];
      /* EventOptionPlugin is unused actually */
    case bEventOptionPlugin:
      [[fallthrough]];
    case bEventVssInitializeForBackup:
      [[fallthrough]];
    case bEventVssInitializeForRestore:
      [[fallthrough]];
    case bEventVssSetBackupState:
      [[fallthrough]];
    case bEventVssPrepareForBackup:
      [[fallthrough]];
    case bEventVssBackupAddComponents:
      [[fallthrough]];
    case bEventVssPrepareSnapshot:
      [[fallthrough]];
    case bEventVssCreateSnapshots:
      [[fallthrough]];
    case bEventVssRestoreLoadComponentMetadata:
      [[fallthrough]];
    case bEventVssRestoreSetComponentsSelected:
      [[fallthrough]];
    case bEventVssCloseRestore:
      [[fallthrough]];
    case bEventVssBackupComplete:
      return simple_event{};

    case bEventLevel:
      return int_event{reinterpret_cast<intptr_t>(data)};

    case bEventRestoreObject:
      return rop_event{reinterpret_cast<restore_object_pkt*>(data)};

    case bEventSince:
      return time_event{reinterpret_cast<time_t>(data)};

    case bEventHandleBackupFile:
      return save_event{reinterpret_cast<save_pkt*>(data)};

    default:
      /* TODO: how best to report an error here ? Maybe return an optional ? */
      DebugLog(10, FMT_STRING("this should really not happen. event = {}"),
               event->eventType);
      __builtin_unreachable();
  }
}

std::optional<std::pair<Socket, Socket>> unix_socket_pair()
{
  int sockets[2];

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
    DebugLog(50, FMT_STRING("could not create socket pair: {}"),
             strerror(errno));
    return std::nullopt;
  }

  return std::make_pair(sockets[0], sockets[1]);
}

std::optional<pid_t> StartDuplexGrpc(std::string_view program_path,
                                     Socket in,
                                     Socket out)
{
  DebugLog(100, FMT_STRING("trying to start {} with sockets {} & {}"),
           program_path, in.get(), out.get());

  pid_t child = fork();

  if (child < 0) {
    return std::nullopt;
  } else if (child == 0) {
    std::string copy(program_path);

    std::string in_str = std::to_string(in.get());
    std::string out_str = std::to_string(out.get());

    char* argv[] = {copy.data(), in_str.data(), out_str.data(), nullptr};
    char* envp[] = {nullptr};

    execve(copy.c_str(), argv, envp);

    // execve only returns if the new process could not be started!
    exit(99);
  } else {
    DebugLog(100, FMT_STRING("Child pid = {}"), child);

    return child;
  }
}

// struct PluginService : bareos::plugin::Plugin::Service {
//   grpc::Status handlePluginEvent(grpc::ServerContext*,
//                                  const
//                                  bareos::plugin::handlePluginEventRequest*
//                                  req,
//                                  bareos::plugin::handlePluginEventResponse*
//                                  resp
//                                 ) override {
//     return grpc::Status::CANCELLED;
//   }
// };


namespace {
namespace bp = bareos::plugin;

class PluginClient {
 public:
  PluginClient(std::shared_ptr<grpc::ChannelInterface> channel)
      : stub_(bp::Plugin::NewStub(channel))
  {
  }

  bRC handlePluginEvent(filedaemon::bEventType type, void* data)
  {
    bp::handlePluginEventRequest req;
    auto* event = req.mutable_to_handle();

    switch (type) {
      case filedaemon::bEventJobStart: {
        auto* inner = event->mutable_job_start();
        inner->set_data((char*)data);
      } break;
      case filedaemon::bEventJobEnd: {
        auto* inner = event->mutable_job_end();
        (void)inner;
      } break;
      case filedaemon::bEventStartBackupJob: {
        auto* inner = event->mutable_start_backup_job();
        (void)inner;
      } break;
      case filedaemon::bEventEndBackupJob: {
        auto* inner = event->mutable_end_backup_job();
        (void)inner;
      } break;
      case filedaemon::bEventStartRestoreJob: {
        auto* inner = event->mutable_start_restore_job();
        (void)inner;
      } break;
      case filedaemon::bEventEndRestoreJob: {
        auto* inner = event->mutable_end_restore_job();
        (void)inner;
      } break;
      case filedaemon::bEventStartVerifyJob: {
        auto* inner = event->mutable_start_verify_job();
        (void)inner;
      } break;
      case filedaemon::bEventEndVerifyJob: {
        auto* inner = event->mutable_end_verify_job();
        (void)inner;
      } break;
      case filedaemon::bEventBackupCommand: {
        auto* inner = event->mutable_backup_command();
        inner->set_data((char*)data);
      } break;
      case filedaemon::bEventRestoreCommand: {
        auto* inner = event->mutable_restore_command();
        inner->set_data((char*)data);
      } break;
      case filedaemon::bEventEstimateCommand: {
        auto* inner = event->mutable_estimate_command();
        inner->set_data((char*)data);
      } break;
      case filedaemon::bEventLevel: {
        auto* inner = event->mutable_level();
        inner->set_level((intptr_t)data);
      } break;
      case filedaemon::bEventSince: {
        auto* inner = event->mutable_since();
        time_t since_time = (intptr_t)data;
        inner->mutable_since()->set_seconds(since_time);
      } break;
      case filedaemon::bEventCancelCommand: {
        auto* inner = event->mutable_cancel_command();
        (void)inner;
      } break;
      case filedaemon::bEventRestoreObject: {
        auto* inner = event->mutable_restore_object();
        // TODO
        (void)inner;
      } break;
      case filedaemon::bEventEndFileSet: {
        auto* inner = event->mutable_end_fileset();
        (void)inner;
      } break;
      case filedaemon::bEventPluginCommand: {
        auto* inner = event->mutable_plugin_command();
        inner->set_data((char*)data);
      } break;
      case filedaemon::bEventOptionPlugin: {
        auto* inner = event->mutable_option_plugin();
        (void)inner;
      } break;
      case filedaemon::bEventHandleBackupFile: {
        auto* inner = event->mutable_handle_backup_file();
        // TODO
        (void)inner;
      } break;
      case filedaemon::bEventNewPluginOptions: {
        auto* inner = event->mutable_new_plugin_options();
        inner->set_data((char*)data);
      } break;
      case filedaemon::bEventVssInitializeForBackup: {
        auto* inner = event->mutable_vss_init_backup();
        (void)inner;
      } break;
      case filedaemon::bEventVssInitializeForRestore: {
        auto* inner = event->mutable_vss_init_restore();
        (void)inner;
      } break;
      case filedaemon::bEventVssSetBackupState: {
        auto* inner = event->mutable_vss_set_backup_state();
        (void)inner;
      } break;
      case filedaemon::bEventVssPrepareForBackup: {
        auto* inner = event->mutable_vss_prepare_snapshot();
        (void)inner;
      } break;
      case filedaemon::bEventVssBackupAddComponents: {
        auto* inner = event->mutable_vss_backup_add_components();
        (void)inner;
      } break;
      case filedaemon::bEventVssPrepareSnapshot: {
        auto* inner = event->mutable_vss_prepare_snapshot();
        (void)inner;
      } break;
      case filedaemon::bEventVssCreateSnapshots: {
        auto* inner = event->mutable_vss_create_snapshot();
        (void)inner;
      } break;
      case filedaemon::bEventVssRestoreLoadComponentMetadata: {
        auto* inner = event->mutable_vss_restore_load_companents_metadata();
        (void)inner;
      } break;
      case filedaemon::bEventVssRestoreSetComponentsSelected: {
        auto* inner = event->mutable_vss_restore_set_components_selected();
        (void)inner;
      } break;
      case filedaemon::bEventVssCloseRestore: {
        auto* inner = event->mutable_vss_close_restore();
        (void)inner;
      } break;
      case filedaemon::bEventVssBackupComplete: {
        auto* inner = event->mutable_vss_backup_complete();
        (void)inner;
      } break;
    }

    bp::handlePluginEventResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->handlePluginEvent(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC startBackupFile(bool portable,
                      bool no_read,
                      std::string_view flags,
                      std::string_view cmd)
  {
    bp::startBackupFileRequest req;
    req.set_no_read(no_read);
    req.set_portable(portable);
    req.set_cmd(cmd.data(), cmd.size());
    req.set_flags(flags.data(), flags.size());

    bp::startBackupFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->startBackupFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    switch (resp.result()) {
      case bareos::plugin::SBF_OK:
        return bRC_OK;
      case bareos::plugin::SBF_Stop:
        return bRC_Stop;
      case bareos::plugin::SBF_Skip:
        return bRC_Skip;
      default:
        return bRC_Error;
    }
  }

  bRC endBackupFile()
  {
    bp::endBackupFileRequest req;

    bp::endBackupFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->endBackupFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    switch (resp.result()) {
      case bareos::plugin::EBF_Done:
        return bRC_OK;
      case bareos::plugin::EBF_More:
        return bRC_More;
      default:
        return bRC_Error;
    }
  }

  bRC startRestoreFile(std::string_view cmd)
  {
    bp::startRestoreFileRequest req;
    req.set_command(cmd.data(), cmd.size());

    bp::startRestoreFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->startRestoreFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC endRestoreFile()
  {
    bp::endRestoreFileRequest req;

    bp::endRestoreFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->endRestoreFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }


  bRC FileOpen(std::string_view name, int32_t flags, int32_t mode)
  {
    bp::fileOpenRequest req;
    req.set_mode(mode);
    req.set_flags(flags);
    req.set_file(name.data(), name.size());

    bp::fileOpenResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->FileOpen(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC FileSeek(int whence, int64_t offset)
  {
    bp::SeekStart start;
    switch (whence) {
      case SEEK_SET: {
        start = bp::SS_StartOfFile;
      } break;
      case SEEK_CUR: {
        start = bp::SS_CurrentPos;
      } break;
      case SEEK_END: {
        start = bp::SS_EndOfFile;
      } break;
      default: {
        return bRC_Error;
      }
    }

    bp::fileSeekRequest req;
    req.set_whence(start);
    req.set_offset(offset);

    bp::fileSeekResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->FileSeek(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC FileRead(char* buffer, size_t size, size_t* num_bytes_read)
  {
    bp::fileReadRequest req;
    req.set_num_bytes(size);

    bp::fileReadResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->FileRead(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    auto& cnt = resp.content();
    *num_bytes_read = cnt.size();

    // ASSERT(cnt.size() <= size);

    memcpy(buffer, cnt.data(), cnt.size());

    return bRC_OK;
  }

  bRC FileWrite(char* buffer, size_t size, size_t* num_bytes_written)
  {
    bp::fileWriteRequest req;
    req.set_content(buffer, size);

    bp::fileWriteResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->FileWrite(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    *num_bytes_written = resp.bytes_written();

    // ASSERT(num_bytes_written <= size);

    return bRC_OK;
  }

  bRC FileClose()
  {
    bp::fileCloseRequest req;

    bp::fileCloseResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->FileClose(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC createFile(filedaemon::restore_pkt* pkt)
  {
    bp::createFileRequest req;
    auto* grpc_pkt = req.mutable_pkt();

    grpc_pkt->set_stream_id(pkt->stream);
    grpc_pkt->set_data_stream(pkt->data_stream);
    grpc_pkt->set_type(pkt->type);
    grpc_pkt->set_file_index(pkt->file_index);
    grpc_pkt->set_link_fi(pkt->LinkFI);
    grpc_pkt->set_user_id(pkt->uid);
    grpc_pkt->set_stat((char*)&pkt->statp, sizeof(pkt->statp));
    grpc_pkt->set_extended_attributes(pkt->attrEx);
    grpc_pkt->set_ofname(pkt->ofname);
    grpc_pkt->set_olname(pkt->olname);
    grpc_pkt->set_where(pkt->where);
    grpc_pkt->set_regex_where(pkt->RegexWhere);
    grpc_pkt->set_replace(pkt->replace);
    grpc_pkt->set_delta_seq(pkt->delta_seq);

    bp::createFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->createFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    switch (resp.status()) {
      case bareos::plugin::CF_Created: {
        pkt->create_status = CF_CREATED;
      } break;
      case bareos::plugin::CF_Extract: {
        pkt->create_status = CF_EXTRACT;
      } break;
      case bareos::plugin::CF_Skip: {
        pkt->create_status = CF_SKIP;
      } break;
      case bareos::plugin::CF_Core: {
        pkt->create_status = CF_CORE;
      } break;
      case bareos::plugin::CF_Error: {
        pkt->create_status = CF_ERROR;
      } break;
      default:
        return bRC_Term;
    }

    return bRC_OK;
  }
  bRC setFileAttributes(std::string_view name,
                        const struct stat& statp,
                        std::string_view extended_attributes,
                        uid_t user_id)
  {
    bp::setFileAttributesRequest req;
    req.set_file(name.data(), name.size());
    req.set_stats((char*)&statp, sizeof(statp));
    req.set_extended_attributes(extended_attributes.data(),
                                extended_attributes.size());
    req.set_user_id(user_id);

    bp::setFileAttributesResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->setFileAttributes(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }
  bRC checkFile(std::string_view name)
  {
    bp::checkFileRequest req;
    req.set_file(name.data(), name.size());
    bp::checkFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->checkFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    if (resp.seen()) { return bRC_Seen; }

    return bRC_OK;
  }

  bRC setAcl(std::string_view file, std::string_view content)
  {
    bp::setAclRequest req;
    req.set_file(file.data(), file.size());
    req.mutable_content()->set_data(content.data(), content.size());

    bp::setAclResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->setAcl(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC getAcl(std::string_view file, char** buffer, size_t* size)
  {
    bp::getAclRequest req;
    req.set_file(file.data(), file.size());

    bp::getAclResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->getAcl(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    auto& data = resp.content().data();
    *buffer = reinterpret_cast<char*>(malloc(data.size() + 1));
    *size = data.size();

    memcpy(buffer, data.data(), data.size() + 1);

    return bRC_OK;
  }

  bRC setXattr(std::string_view file,
               std::string_view key,
               std::string_view value)
  {
    bp::setXattrRequest req;
    req.set_file(file.data(), file.size());
    auto* xattr = req.mutable_attribute();
    xattr->set_key(key.data(), key.size());
    xattr->set_value(value.data(), value.size());

    bp::setXattrResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->setXattr(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
  }

  bRC getXattr(std::string_view file,
               char** name_buf,
               size_t* name_size,
               char** value_buf,
               size_t* value_size)
  {
    // The idea here is that we grab all xattributes at once
    // and then trickle them out for each call

    if (current_xattr_index == std::numeric_limits<size_t>::max()) {
      // we need to grab them now
      bp::getXattrRequest req;
      req.set_file(file.data(), file.size());

      bp::getXattrResponse resp;
      grpc::ClientContext ctx;
      grpc::Status status = stub_->getXattr(&ctx, req, &resp);

      if (!status.ok()) { return bRC_Error; }

      xattribute_cache.assign(
          std::make_move_iterator(std::begin(resp.attributes())),
          std::make_move_iterator(std::end(resp.attributes())));

      current_xattr_index = 0;
    }

    if (current_xattr_index == xattribute_cache.size()) {
      current_xattr_index = -1;  // reset
      return bRC_OK;
    }

    auto& current = xattribute_cache[current_xattr_index++];

    auto& key = current.key();
    auto& val = current.value();

    *name_size = key.size();
    *name_buf = reinterpret_cast<char*>(malloc(key.size() + 1));
    memcpy(name_buf, key.data(), key.size() + 1);

    *value_size = val.size();
    *value_buf = reinterpret_cast<char*>(malloc(val.size() + 1));
    memcpy(value_buf, val.data(), val.size() + 1);

    return bRC_More;
  }

 private:
  std::unique_ptr<bp::Plugin::Stub> stub_;

  size_t current_xattr_index{std::numeric_limits<size_t>::max()};
  std::vector<bp::Xattribute> xattribute_cache;
};

namespace bc = bareos::core;

class BareosEvents : public bc::Events::Service {
  grpc::Status Register(grpc::ServerContext*,
                        const bc::RegisterRequest* req,
                        bc::RegisterResponse* resp) override
  {
    (void)req;
    (void)resp;
    return grpc::Status::CANCELLED;
  }

  grpc::Status Unregister(grpc::ServerContext*,
                          const bc::UnregisterRequest* req,
                          bc::UnregisterResponse* resp) override
  {
    (void)req;
    (void)resp;
    return grpc::Status::CANCELLED;
  }
};
}  // namespace

// namespace {
struct grpc_connection_members {
  PluginClient client;
  std::unique_ptr<grpc::Server> server;
  std::vector<std::unique_ptr<grpc::Service>> services;

  grpc_connection_members() = delete;
};


struct connection_builder {
  std::optional<PluginClient> opt_client;
  std::optional<std::unique_ptr<grpc::Server>> opt_server;
  std::vector<std::unique_ptr<grpc::Service>> services;

  template <typename... Args>
  connection_builder(Args... args) : services{std::forward<Args>(args)...}
  {
  }

  connection_builder& connect_client(Socket s)
  {
    auto* channel = grpc_channel_create_from_fd(
        "plugin", s.get(), grpc_insecure_credentials_create(), nullptr);

    if (channel) {
      DebugLog(100, FMT_STRING("could connect to client over socket {}"),
               s.get());

      s.release();

      opt_client.emplace(grpc::CreateChannelInternal("", channel, {}));
    } else {
      DebugLog(50, FMT_STRING("could not connect to client over socket {}"),
               s.get());
    }

    return *this;
  }

  connection_builder& connect_server(Socket s)
  {
    try {
      grpc::ServerBuilder builder;

      for (auto& service : services) { builder.RegisterService(service.get()); }

      auto ccserver = builder.BuildAndStart();

      auto* server = ccserver->c_server();

      grpc_server_add_channel_from_fd(
          server, s.get(), grpc_insecure_server_credentials_create());
    } catch (const std::exception& e) {
      DebugLog(50, FMT_STRING("could not attach socket {} to server: Err={}"),
               s.get(), e.what());
      opt_server = std::nullopt;
    } catch (...) {
      opt_server = std::nullopt;
    }
    return *this;
  }


  std::optional<grpc_connection> build()
  {
    if (!opt_client) { return std::nullopt; }
    if (!opt_server) { return std::nullopt; }

    grpc_connection con{};

    con.members = new grpc_connection_members{std::move(opt_client.value()),
                                              std::move(opt_server.value()),
                                              std::move(services)};

    return con;
  }
};
//};  // namespace

std::optional<grpc_connection> make_connection_from(Socket in, Socket out)
{
  return connection_builder{std::make_unique<BareosEvents>()}
      .connect_client(std::move(out))
      .connect_server(std::move(in))
      .build();
}

std::optional<grpc_connection> make_connection(std::string_view program_path)
{
  // We want to create a two way grpc connection, where both ends act as both
  // a server and a client.  We do this by using two socket pairs.

  DebugLog(100, FMT_STRING("creating connection to {} ..."), program_path);

  auto to_program = unix_socket_pair();
  auto from_program = unix_socket_pair();

  if (!to_program || !from_program) {
    DebugLog(50,
             FMT_STRING("abort creation of connection to {} as socket pairs "
                        "could not be created"),
             program_path);
    return std::nullopt;
  }

  if (fcntl(to_program->first.get(), F_SETFD, FD_CLOEXEC) < 0) {
    DebugLog(
        50,
        FMT_STRING("could not set CLOEXEC on program input socket {}. Err={}"),
        to_program->first.get(), strerror(errno));
  }

  if (fcntl(from_program->first.get(), F_SETFD, FD_CLOEXEC) < 0) {
    DebugLog(
        50,
        FMT_STRING("could not set CLOEXEC on program output socket {}. Err={}"),
        from_program->first.get(), strerror(errno));
  }

  auto child = StartDuplexGrpc(program_path, std::move(to_program->second),
                               std::move(from_program->second));

  if (!child) {
    DebugLog(50,
             FMT_STRING("abort creation of connection to {} as program could "
                        "not get started"),
             program_path);
    return std::nullopt;
  }

  errno = 0;
  char test[] = "Hallo, Welt!";

  // todo: we need to somehow select() on the events of the child
  //       and the input/output sockets, so that we do not cause any deadlocks
  //       waiting for a write of an already dead child
  DebugLog(100, FMT_STRING("writing to {}"), to_program->first.get());
  if (auto rc = write(to_program->first.get(), test, sizeof(test));
      rc != sizeof(test)) {
    DebugLog(50, FMT_STRING("bad write: rc = {} ({})"), rc, strerror(errno));
    return std::nullopt;
  }

  DebugLog(100, FMT_STRING("wrote {} to {}"),
           std::string_view{test, sizeof(test) - 1}, to_program->first.get());

  DebugLog(100, FMT_STRING("reading from {}"), from_program->first.get());

  char read_buffer[sizeof(test)];
  auto read_bytes
      = read(from_program->first.get(), read_buffer, sizeof(read_buffer));
  DebugLog(100, FMT_STRING("read {} bytes from {}"), read_bytes,
           from_program->first.get());
  if (read_bytes != (ssize_t)sizeof(read_buffer)) {
    DebugLog(50, FMT_STRING("bad read: rc = {} ({})"), read_bytes,
             strerror(errno));
    return std::nullopt;
  }

  if (memcmp(test, read_buffer, sizeof(test)) != 0) {
    DebugLog(50, FMT_STRING("bad data: test = {}"),
             std::string_view{read_buffer, sizeof(read_buffer)});
    return std::nullopt;
  }

  {
    bareos::plugin::RestorePacket orig;
    orig.set_file_index(3434);
    {
      auto out_stream
          = google::protobuf::io::FileOutputStream(to_program->first.get());
      google::protobuf::io::CodedOutputStream s{&out_stream};

      size_t len = orig.ByteSizeLong();
      s.WriteRaw(&len, sizeof(len));
      DebugLog(100, FMT_STRING("wrote len = {}"), len);
      if (!orig.SerializeToCodedStream(&s)) {
        DebugLog(50, FMT_STRING("could not serialize input"));
        goto next_step;
      }
      DebugLog(100, FMT_STRING("wrote obj"));
    }

    {
      auto in_stream
          = google::protobuf::io::FileInputStream(from_program->first.get());
      google::protobuf::io::CodedInputStream is{&in_stream};

      size_t len = 0;

      decltype(orig) copy;

      if (!is.ReadRaw(&len, sizeof(len))) {
        DebugLog(50, FMT_STRING("could not read len output"));
        goto next_step;
      }

      DebugLog(100, FMT_STRING("read len = {}"), len);

      std::vector<char> bytes;
      bytes.resize(len);

      is.ReadRaw(bytes.data(), bytes.size());

      if (!copy.ParseFromArray(bytes.data(), bytes.size())) {
        DebugLog(50, FMT_STRING("could not parse output"));
        goto next_step;
      }

      if (orig.file_index() != copy.file_index()) {
        DebugLog(50, FMT_STRING("file_inedx difference detected: {} != {}"),
                 orig.file_index(), copy.file_index());
        goto next_step;
      }

      DebugLog(100, FMT_STRING("everything worked!"));
    }
  }
next_step:

  // {
  //   bareos::plugin::TestRequest in;
  //   in.set_input(16);

  //   DebugLog(100, FMT_STRING("Trying to serialize {} bytes to {}"),
  //            in.ByteSizeLong(), to_program->first.get());
  //   if (!in.SerializeToFileDescriptor(to_program->first.get())) {
  //     DebugLog(50, FMT_STRING("could not serialize input"));
  //     goto next_step;
  //   }

  //   bareos::plugin::TestRequest out;

  //   DebugLog(50, FMT_STRING("trying to parse from {} (current size = {})"),
  //            from_program->first.get(), out.ByteSizeLong());

  //   char protobuf[200];


  //   auto proto_bytes
  //       = read(from_program->first.get(), protobuf, sizeof(protobuf));

  //   if (proto_bytes < 0) {
  //     DebugLog(50, FMT_STRING("could not read from {}. Err={}"),
  //              from_program->first.get(), strerror(errno));
  //   } else {
  //     DebugLog(100, FMT_STRING("read {} bytes from {}"), proto_bytes,
  //              from_program->first.get());
  //   }

  //   if (!out.ParseFromArray(protobuf, proto_bytes)) {
  //     DebugLog(50, FMT_STRING("could not parse output"));
  //     goto next_step;
  //   }

  //   if (out.input() != in.input()) {
  //     DebugLog(50, FMT_STRING("did not parse correctly: {} != {}"),
  //     in.input(),
  //              out.input());
  //   } else {
  //     DebugLog(100, FMT_STRING("sucessfully parsed {}"), out.input());
  //   }
  // }
  // next_step:

  auto con = make_connection_from(std::move(from_program->first),
                                  std::move(to_program->first));

  // wait for the child to close (for now)
  for (;;) {
    int status = 0;
    if (waitpid(*child, &status, 0) < 0) {
      DebugLog(50, FMT_STRING("wait pid failed. Err={}"), strerror(errno));
    }

    if (WIFEXITED(status)) {
      DebugLog(100, FMT_STRING("child exit status = {}"), WEXITSTATUS(status));

      break;
    } else {
      DebugLog(100, FMT_STRING("got status = {}"), status);
    }
  }

  return con;
}

grpc_connection::~grpc_connection() { delete members; }
bRC grpc_connection::startBackupFile(filedaemon::save_pkt* pkt)
{
  PluginClient* client = &members->client;
  return client->startBackupFile(
      pkt->portable, pkt->no_read,
      std::string_view{pkt->flags, sizeof(pkt->flags)}, pkt->cmd);
}
bRC grpc_connection::endBackupFile()
{
  PluginClient* client = &members->client;
  return client->endBackupFile();
}
bRC grpc_connection::startRestoreFile(std::string_view cmd)
{
  PluginClient* client = &members->client;
  return client->startRestoreFile(cmd);
}
bRC grpc_connection::endRestoreFile()
{
  PluginClient* client = &members->client;
  return client->endRestoreFile();
}
bRC grpc_connection::pluginIO(filedaemon::io_pkt* pkt)
{
  PluginClient* client = &members->client;
  switch (pkt->func) {
    case filedaemon::IO_OPEN: {
      return client->FileOpen(pkt->fname, pkt->flags, pkt->mode);
    } break;
    case filedaemon::IO_READ: {
      size_t count = 0;
      auto res = client->FileRead(pkt->buf, pkt->count, &count);
      pkt->status = count;
      return res;
    } break;
    case filedaemon::IO_WRITE: {
      size_t count = 0;
      auto res = client->FileWrite(pkt->buf, pkt->count, &count);
      pkt->status = count;
      return res;
    } break;
    case filedaemon::IO_CLOSE: {
      return client->FileClose();
    } break;
    case filedaemon::IO_SEEK: {
      return client->FileSeek(pkt->whence, pkt->offset);
    } break;
    default: {
      return bRC_Error;
    } break;
  }
}
bRC grpc_connection::createFile(filedaemon::restore_pkt* pkt)
{
  PluginClient* client = &members->client;
  return client->createFile(pkt);
}
bRC grpc_connection::setFileAttributes(filedaemon::restore_pkt* pkt)
{
  PluginClient* client = &members->client;
  return client->setFileAttributes(pkt->ofname, pkt->statp, pkt->attrEx,
                                   pkt->uid);
}
bRC grpc_connection::checkFile(const char* fname)
{
  PluginClient* client = &members->client;
  return client->checkFile(fname);
}
bRC grpc_connection::getAcl(filedaemon::acl_pkt* pkt)
{
  PluginClient* client = &members->client;
  size_t size = 0;
  bRC result = client->getAcl(pkt->fname, &pkt->content, &size);
  pkt->content_length = size;
  return result;
}
bRC grpc_connection::setAcl(filedaemon::acl_pkt* pkt)
{
  PluginClient* client = &members->client;
  return client->setAcl(pkt->fname,
                        std::string_view{pkt->content, pkt->content_length});
}
bRC grpc_connection::getXattr(filedaemon::xattr_pkt* pkt)
{
  PluginClient* client = &members->client;
  size_t name_len = 0, value_len = 0;

  bRC result = client->getXattr(pkt->fname, &pkt->name, &name_len, &pkt->value,
                                &value_len);

  pkt->name_length = name_len;
  pkt->value_length = value_len;

  return result;
}
bRC grpc_connection::setXattr(filedaemon::xattr_pkt* pkt)
{
  PluginClient* client = &members->client;

  return client->setXattr(pkt->fname,
                          std::string_view{pkt->name, pkt->name_length},
                          std::string_view{pkt->value, pkt->value_length});
}
