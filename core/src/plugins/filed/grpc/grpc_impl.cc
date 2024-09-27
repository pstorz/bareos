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

#include "common.pb.h"
#include "events.pb.h"
#include "plugin.grpc.pb.h"
#include "plugin.pb.h"
#include "bareos.grpc.pb.h"
#include "bareos.pb.h"
#include "filed/fd_plugins.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_posix.h>
#include <grpcpp/create_channel_posix.h>

#include "plugins/filed/grpc/grpc_impl.h"
#include <fcntl.h>
#include <thread>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/security/server_credentials.h>
#include <sys/wait.h>

#include "bareos_api.h"

#include "include/filetypes.h"

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
                                     Socket out,
                                     Socket io)
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
    std::string io_str = std::to_string(io.get());

    char* argv[]
        = {copy.data(), in_str.data(), out_str.data(), io_str.data(), nullptr};
    char* envp[] = {nullptr};

    execve(copy.c_str(), argv, envp);

    // execve only returns if the new process could not be started!
    exit(99);
  } else {
    DebugLog(100, FMT_STRING("Child pid = {}"), child);

    return child;
  }
}

namespace {
namespace bp = bareos::plugin;

namespace bc = bareos::core;
namespace bco = bareos::common;

class BareosCore : public bc::Core::Service {
 public:
  BareosCore(PluginContext* ctx) : core{ctx} {}

  static std::optional<filedaemon::bEventType> from_grpc(bc::EventType type)
  {
    switch (type) {
      case bc::Event_JobStart:
        return filedaemon::bEventJobStart;
      case bc::Event_JobEnd:
        return filedaemon::bEventJobEnd;
      case bc::Event_StartBackupJob:
        return filedaemon::bEventStartBackupJob;
      case bc::Event_EndBackupJob:
        return filedaemon::bEventEndBackupJob;
      case bc::Event_StartRestoreJob:
        return filedaemon::bEventStartRestoreJob;
      case bc::Event_EndRestoreJob:
        return filedaemon::bEventEndRestoreJob;
      case bc::Event_StartVerifyJob:
        return filedaemon::bEventStartVerifyJob;
      case bc::Event_EndVerifyJob:
        return filedaemon::bEventEndVerifyJob;
      case bc::Event_BackupCommand:
        return filedaemon::bEventBackupCommand;
      case bc::Event_RestoreCommand:
        return filedaemon::bEventRestoreCommand;
      case bc::Event_EstimateCommand:
        return filedaemon::bEventEstimateCommand;
      case bc::Event_Level:
        return filedaemon::bEventLevel;
      case bc::Event_Since:
        return filedaemon::bEventSince;
      case bc::Event_CancelCommand:
        return filedaemon::bEventCancelCommand;
      case bc::Event_RestoreObject:
        return filedaemon::bEventRestoreObject;
      case bc::Event_EndFileSet:
        return filedaemon::bEventEndFileSet;
      case bc::Event_PluginCommand:
        return filedaemon::bEventPluginCommand;
      case bc::Event_OptionPlugin:
        return filedaemon::bEventOptionPlugin;
      case bc::Event_HandleBackupFile:
        return filedaemon::bEventHandleBackupFile;
      case bc::Event_NewPluginOptions:
        return filedaemon::bEventNewPluginOptions;
      case bc::Event_VssInitializeForBackup:
        return filedaemon::bEventVssInitializeForBackup;
      case bc::Event_VssInitializeForRestore:
        return filedaemon::bEventVssInitializeForRestore;
      case bc::Event_VssSetBackupState:
        return filedaemon::bEventVssSetBackupState;
      case bc::Event_VssPrepareForBackup:
        return filedaemon::bEventVssPrepareForBackup;
      case bc::Event_VssBackupAddComponents:
        return filedaemon::bEventVssBackupAddComponents;
      case bc::Event_VssPrepareSnapshot:
        return filedaemon::bEventVssPrepareSnapshot;
      case bc::Event_VssCreateSnapshots:
        return filedaemon::bEventVssCreateSnapshots;
      case bc::Event_VssRestoreLoadComponentMetadata:
        return filedaemon::bEventVssRestoreLoadComponentMetadata;
      case bc::Event_VssRestoreSetComponentsSelected:
        return filedaemon::bEventVssRestoreSetComponentsSelected;
      case bc::Event_VssCloseRestore:
        return filedaemon::bEventVssCloseRestore;
      case bc::Event_VssBackupComplete:
        return filedaemon::bEventVssBackupComplete;
      default:
        return std::nullopt;
    }
  }

  static std::optional<int> from_grpc(bco::FileType type)
  {
    switch (type) {
      case bco::FT_LNKSAVED:
        return FT_LNKSAVED;
      case bco::FT_REGE:
        return FT_REGE;
      case bco::FT_REG:
        return FT_REG;
      case bco::FT_LNK:
        return FT_LNK;
      case bco::FT_DIREND:
        return FT_DIREND;
      case bco::FT_SPEC:
        return FT_SPEC;
      case bco::FT_NOACCESS:
        return FT_NOACCESS;
      case bco::FT_NOFOLLOW:
        return FT_NOFOLLOW;
      case bco::FT_NOSTAT:
        return FT_NOSTAT;
      case bco::FT_NOCHG:
        return FT_NOCHG;
      case bco::FT_DIRNOCHG:
        return FT_DIRNOCHG;
      case bco::FT_ISARCH:
        return FT_ISARCH;
      case bco::FT_NORECURSE:
        return FT_NORECURSE;
      case bco::FT_NOFSCHG:
        return FT_NOFSCHG;
      case bco::FT_NOOPEN:
        return FT_NOOPEN;
      case bco::FT_RAW:
        return FT_RAW;
      case bco::FT_FIFO:
        return FT_FIFO;
      case bco::FT_DIRBEGIN:
        return FT_DIRBEGIN;
      case bco::FT_INVALIDFS:
        return FT_INVALIDFS;
      case bco::FT_INVALIDDT:
        return FT_INVALIDDT;
      case bco::FT_REPARSE:
        return FT_REPARSE;
      case bco::FT_PLUGIN:
        return FT_PLUGIN;
      case bco::FT_DELETED:
        return FT_DELETED;
      case bco::FT_BASE:
        return FT_BASE;
      case bco::FT_RESTORE_FIRST:
        return FT_RESTORE_FIRST;
      case bco::FT_JUNCTION:
        return FT_JUNCTION;
      case bco::FT_PLUGIN_CONFIG:
        return FT_PLUGIN_CONFIG;
      case bco::FT_PLUGIN_CONFIG_FILLED:
        return FT_PLUGIN_CONFIG_FILLED;
      default:
        return std::nullopt;
    }
  }

  static std::optional<filedaemon::bVariable> from_grpc(
      bc::BareosStringVariable var)
  {
    switch (var) {
      case bc::BV_FDName:
        return filedaemon::bVarFDName;
      case bc::BV_ClientName:
        return filedaemon::bVarClient;
      case bc::BV_JobName:
        return filedaemon::bVarJobName;
      case bc::BV_WorkingDir:
        return filedaemon::bVarWorkingDir;
      case bc::BV_Where:
        return filedaemon::bVarWhere;
      case bc::BV_RegexWhere:
        return filedaemon::bVarRegexWhere;
      case bc::BV_ExePath:
        return filedaemon::bVarExePath;
      case bc::BV_BareosVersion:
        return filedaemon::bVarVersion;
      case bc::BV_PreviousJobName:
        return filedaemon::bVarPrevJobName;
      case bc::BV_UsedConfig:
        return filedaemon::bVarUsedConfig;
      case bc::BV_PluginPath:
        return filedaemon::bVarPluginPath;
      default:
        return std::nullopt;
    }
  }

  static std::optional<filedaemon::bVariable> from_grpc(
      bc::BareosIntVariable var)
  {
    switch (var) {
      case bc::BV_JobId:
        return filedaemon::bVarJobId;
      case bc::BV_JobLevel:
        return filedaemon::bVarLevel;
      case bc::BV_JobType:
        return filedaemon::bVarType;
      case bc::BV_JobStatus:
        return filedaemon::bVarJobStatus;
      case bc::BV_SinceTime:
        return filedaemon::bVarSinceTime;
      case bc::BV_Accurate:
        return filedaemon::bVarAccurate;
      case bc::BV_PrefixLinks:
        return filedaemon::bVarPrefixLinks;
      default:
        return std::nullopt;
    }
  }

  static std::optional<filedaemon::bVariable> from_grpc(
      bc::BareosFlagVariable var)
  {
    switch (var) {
      case bc::BV_FileSeen:
        return filedaemon::bVarFileSeen;
      case bc::BV_CheckChanges:
        return filedaemon::bVarCheckChanges;
      default:
        return std::nullopt;
    }
  }


 private:
  grpc::Status Events_Register(grpc::ServerContext*,
                               const bc::RegisterRequest* req,
                               bc::RegisterResponse*) override
  {
    for (auto event : req->event_types()) {
      if (!bc::EventType_IsValid(event)) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            fmt::format(FMT_STRING("event {} is not a valid bareos event"),
                        event));
      }
    }

    for (auto event : req->event_types()) {
      // for some reason event is an int ??
      std::optional bareos_type = from_grpc(static_cast<bc::EventType>(event));
      if (!bareos_type) {
        return grpc::Status(
            grpc::StatusCode::INTERNAL,
            fmt::format(
                FMT_STRING("could not convert valid event {} to bareos event"),
                event));
      }

      RegisterBareosEvent(core, *bareos_type);
    }

    return grpc::Status::OK;
  }

  grpc::Status Events_Unregister(grpc::ServerContext*,
                                 const bc::UnregisterRequest* req,
                                 bc::UnregisterResponse*) override
  {
    for (auto event : req->event_types()) {
      if (!bc::EventType_IsValid(event)) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            fmt::format(FMT_STRING("event {} is not a valid bareos event"),
                        event));
      }
    }

    for (auto event : req->event_types()) {
      // for some reason event is an int ??
      std::optional bareos_type = from_grpc(static_cast<bc::EventType>(event));
      if (!bareos_type) {
        return grpc::Status(
            grpc::StatusCode::INTERNAL,
            fmt::format(
                FMT_STRING("could not convert valid event {} to bareos event"),
                event));
      }

      UnregisterBareosEvent(core, *bareos_type);
    }

    return grpc::Status::OK;
  }

  // grpc::Status Fileset_AddExclude(grpc::ServerContext*, const
  // bc::AddExcludeRequest* request, bc::AddExcludeResponse* response) override
  // {
  // }
  // grpc::Status Fileset_AddInclude(grpc::ServerContext*, const
  // bc::AddIncludeRequest* request, bc::AddIncludeResponse* response) override
  // {
  // }
  // grpc::Status Fileset_AddOptions(grpc::ServerContext*, const
  // bc::AddOptionsRequest* request, bc::AddOptionsResponse* response) override
  // {
  // }
  // grpc::Status Fileset_AddRegex(grpc::ServerContext*, const
  // bc::AddRegexRequest* request, bc::AddRegexResponse* response) override {
  // }
  // grpc::Status Fileset_AddWild(grpc::ServerContext*, const
  // bc::AddWildRequest* request, bc::AddWildResponse* response) override {
  // }
  // grpc::Status Fileset_NewOptions(grpc::ServerContext*, const
  // bc::NewOptionsRequest* request, bc::NewOptionsResponse* response) override
  // {
  // }
  // grpc::Status Fileset_NewInclude(grpc::ServerContext*, const
  // bc::NewIncludeRequest* request, bc::NewIncludeResponse* response) override
  // {
  // }
  // grpc::Status Fileset_NewPreInclude(grpc::ServerContext*, const
  // bc::NewPreIncludeRequest* request, bc::NewPreIncludeResponse* response)
  // override {
  // }
  grpc::Status Bareos_getInstanceCount(
      grpc::ServerContext*,
      const bc::getInstanceCountRequest*,
      bc::getInstanceCountResponse* response) override
  {
    // there is only one instance per process.  Its also pretty easy to give a
    // real answer here (i guess).
    response->set_instance_count(1);
    return grpc::Status::OK;
  }

  grpc::Status Bareos_checkChanges(grpc::ServerContext*,
                                   const bc::checkChangesRequest* request,
                                   bc::checkChangesResponse* response) override
  {
    auto type = request->type();

    auto bareos_type = from_grpc(type);

    if (!bareos_type) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("could not parse {} as bareos type"),
                      int(type)));
    }

    auto& stats = request->stats();
    struct stat statp;

    if (stats.size() != sizeof(statp)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(
              FMT_STRING(
                  "stats is not a valid stats object: size mismatch {} != {}"),
              stats.size(), sizeof(statp)));
    }

    memcpy(&statp, stats.data(), stats.size());

    auto result
        = checkChanges(core, request->file(), *bareos_type, statp,
                       static_cast<time_t>(request->since_time().seconds()));

    response->set_old(!result);

    return grpc::Status::OK;
  }

  grpc::Status Bareos_SetString(grpc::ServerContext*,
                                const bc::SetStringRequest* request,
                                bc::SetStringResponse*) override
  {
    auto var = request->var();

    std::optional bareos_var = from_grpc(var);

    if (!bareos_var) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("unknown string variable {}"), int(var)));
    }

    if (!SetBareosValue(core, *bareos_var,
                        const_cast<char*>(request->value().c_str()))) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("set not supported for {}"), int(var)));
    }

    return grpc::Status::OK;
  }

  grpc::Status Bareos_GetString(grpc::ServerContext*,
                                const bc::GetStringRequest* request,
                                bc::GetStringResponse* response) override
  {
    auto var = request->var();

    std::optional bareos_var = from_grpc(var);

    if (!bareos_var) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("unknown string variable {}"), int(var)));
    }

    const char* str = nullptr;

    if (!GetBareosValue(core, *bareos_var, &str)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("get not supported for {}"), int(var)));
    }

    if (str == nullptr) {
      return grpc::Status(grpc::StatusCode::INTERNAL,
                          "nullptr returned by core");
    }

    response->set_value(str);

    return grpc::Status::OK;
  }

  grpc::Status Bareos_SetInt(grpc::ServerContext*,
                             const bc::SetIntRequest* request,
                             bc::SetIntResponse*) override
  {
    auto var = request->var();

    std::optional bareos_var = from_grpc(var);

    if (!bareos_var) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("unknown string variable {}"), int(var)));
    }

    int val = request->value();
    if (!SetBareosValue(core, *bareos_var, &val)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("set not supported for {}"), int(var)));
    }

    return grpc::Status::OK;
  }

  grpc::Status Bareos_GetInt(grpc::ServerContext*,
                             const bc::GetIntRequest* request,
                             bc::GetIntResponse* response) override
  {
    auto var = request->var();

    std::optional bareos_var = from_grpc(var);

    if (!bareos_var) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("unknown string variable {}"), int(var)));
    }

    int value{0};

    if (!GetBareosValue(core, *bareos_var, &value)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("get not supported for {}"), int(var)));
    }

    response->set_value(value);

    return grpc::Status::OK;
  }

  grpc::Status Bareos_SetFlag(grpc::ServerContext*,
                              const bc::SetFlagRequest* request,
                              bc::SetFlagResponse*) override
  {
    auto var = request->var();

    std::optional bareos_var = from_grpc(var);

    if (!bareos_var) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("unknown string variable {}"), int(var)));
    }

    bool val = request->value();
    if (!SetBareosValue(core, *bareos_var, &val)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("set not supported for {}"), int(var)));
    }

    return grpc::Status::OK;
  }

  grpc::Status Bareos_GetFlag(grpc::ServerContext*,
                              const bc::GetFlagRequest* request,
                              bc::GetFlagResponse* response) override
  {
    auto var = request->var();

    std::optional bareos_var = from_grpc(var);

    if (!bareos_var) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("unknown string variable {}"), int(var)));
    }

    bool value{false};

    if (!GetBareosValue(core, *bareos_var, &value)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(FMT_STRING("get not supported for {}"), int(var)));
    }

    response->set_value(value);

    return grpc::Status::OK;
  }

  grpc::Status Bareos_AcceptFile(grpc::ServerContext*,
                                 const bc::AcceptFileRequest* request,
                                 bc::AcceptFileResponse* response) override
  {
    auto& stats = request->stats();
    struct stat statp;

    if (stats.size() != sizeof(statp)) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          fmt::format(
              FMT_STRING(
                  "stats is not a valid stats object: size mismatch {} != {}"),
              stats.size(), sizeof(statp)));
    }

    memcpy(&statp, stats.data(), stats.size());

    auto result = AcceptFile(core, request->file(), statp);

    response->set_skip(!result);

    return grpc::Status::OK;
  }
  grpc::Status Bareos_SetSeen(grpc::ServerContext*,
                              const bc::SetSeenRequest* request,
                              bc::SetSeenResponse*) override
  {
    auto result = [&] {
      if (request->has_file()) {
        return SetSeenBitmap(core, false, request->file().c_str());
      } else {
        return SetSeenBitmap(core, true, nullptr);
      }
    }();

    if (result == bRC_Error) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "something went wrong!");
    }
    return grpc::Status::OK;
  }
  grpc::Status Bareos_ClearSeen(grpc::ServerContext*,
                                const bc::ClearSeenRequest* request,
                                bc::ClearSeenResponse*) override
  {
    auto result = [&] {
      if (request->has_file()) {
        return ClearSeenBitmap(core, false, request->file().c_str());
      } else {
        return ClearSeenBitmap(core, true, nullptr);
      }
    }();

    if (result == bRC_Error) {
      return grpc::Status(grpc::StatusCode::INTERNAL, "something went wrong!");
    }
    return grpc::Status::OK;
  }

  grpc::Status Bareos_JobMessage(grpc::ServerContext*,
                                 const bc::JobMessageRequest* req,
                                 bc::JobMessageResponse*) override
  {
    JobLog(core, Type{req->type(), req->file().c_str(), (int)req->line()},
           FMT_STRING("{}"), req->msg());

    return grpc::Status::OK;
  }

  grpc::Status Bareos_DebugMessage(grpc::ServerContext*,
                                   const bc::DebugMessageRequest* req,
                                   bc::DebugMessageResponse*) override
  {
    DebugLog(core,
             Severity{(int)req->level(), req->file().c_str(), (int)req->line()},
             FMT_STRING("{}"), req->msg());

    return grpc::Status::OK;
  }

 private:
  PluginContext* core{nullptr};
};

class PluginClient {
 public:
  PluginClient(std::shared_ptr<grpc::ChannelInterface> channel)
      : stub_(bp::Plugin::NewStub(channel))
  {
  }

  bRC Setup()
  {
    bp::SetupRequest req;
    bp::SetupResponse resp;
    grpc::ClientContext ctx;

    auto status = stub_->Setup(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    return bRC_OK;
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


    if (!status.ok()) {
      DebugLog(50, FMT_STRING("rpc did not succeed for event {} ({}): Err={}"),
               int(type), int(status.error_code()), status.error_message());
      return bRC_Error;
    }

    auto res = resp.res();

    DebugLog(100, FMT_STRING("plugin handled with res = {} ({})"),
             bp::ReturnCode_Name(res), int(res));

    switch (res) {
      case bareos::plugin::RC_OK:
        return bRC_OK;
      case bareos::plugin::RC_Stop:
        return bRC_Stop;
      case bareos::plugin::RC_More:
        return bRC_More;
      case bareos::plugin::RC_Term:
        return bRC_Term;
      case bareos::plugin::RC_Seen:
        return bRC_Seen;
      case bareos::plugin::RC_Core:
        return bRC_Core;
      case bareos::plugin::RC_Skip:
        return bRC_Skip;
      case bareos::plugin::RC_Cancel:
        return bRC_Cancel;

      case bareos::plugin::RC_Error:
        [[fallthrough]];
      default:
        return bRC_Error;
    }
  }

  bRC startBackupFile(filedaemon::save_pkt* pkt)
  {
    bp::startBackupFileRequest req;
    req.set_no_read(pkt->no_read);
    req.set_portable(pkt->portable);
    req.set_cmd(pkt->cmd);
    req.set_flags(pkt->flags, sizeof(pkt->flags));

    bp::startBackupFileResponse resp;
    grpc::ClientContext ctx;
    grpc::Status status = stub_->startBackupFile(&ctx, req, &resp);

    if (!status.ok()) { return bRC_Error; }

    switch (resp.result()) {
      case bareos::plugin::SBF_OK: {
        if (resp.has_file()) {
          auto& file = resp.file();

          DebugLog(100, FMT_STRING("received a file"));

          if (file.stats().size() != sizeof(pkt->statp)) {
            DebugLog(50, FMT_STRING("stats has the wrong size {} != {}"),
                     file.stats().size(), sizeof(pkt->statp));
            return bRC_Error;
          }
          std::optional ft = BareosCore::from_grpc(file.ft());
          if (!ft) {
            DebugLog(50, FMT_STRING("could not convert filetype {} ({})"),
                     bco::FileType_Name(file.ft()), int(file.ft()));
            return bRC_Error;
          }
          if (pkt->fname) {
            free(pkt->fname);
            pkt->fname = nullptr;
          }
          if (pkt->link) {
            free(pkt->link);
            pkt->link = nullptr;
          }
          pkt->fname = strdup(file.file().c_str());
          // TODO: fix this
          pkt->link = strdup(file.file().c_str());
          memcpy(&pkt->statp, file.stats().data(), file.stats().size());
          pkt->type = *ft;
          pkt->no_read = file.no_read();
          pkt->portable = file.portable();

          if (file.has_delta_seq()) {
            SetBit(FO_DELTA, pkt->flags);
            pkt->delta_seq = file.delta_seq();
          } else {
            ClearBit(FO_DELTA, pkt->flags);
            pkt->delta_seq = 0;
          }
          if (file.offset_backup()) {
            SetBit(FO_OFFSETS, pkt->flags);
          } else {
            ClearBit(FO_OFFSETS, pkt->flags);
          }
          if (file.sparse_backup()) {
            SetBit(FO_SPARSE, pkt->flags);
          } else {
            ClearBit(FO_SPARSE, pkt->flags);
          }
          // THIS is unused, so we always clear it to be sure
          ClearBit(FO_PORTABLE_DATA, pkt->flags);
        } else if (resp.has_object()) {
          DebugLog(100, FMT_STRING("received an object"));
          auto& object = resp.object();
          pkt->type = FT_RESTORE_FIRST;
          if (pkt->object) {
            free(pkt->object);
            pkt->object = nullptr;
          }
          if (pkt->object_name) {
            free(pkt->object_name);
            pkt->object_name = nullptr;
          }

          // TODO: this is not cleaned up properly.  The only way to do this
          //       is to use a job-local variable.  Maybe this can be put
          //       into the plugin context ...
          pkt->object = reinterpret_cast<char*>(malloc(object.data().size()));
          memcpy(pkt->object, object.data().c_str(), object.data().size());
          pkt->object_len = object.data().size();
          // TODO: this as well
          pkt->object_name = strdup(object.name().c_str());
          pkt->index = object.index();
        } else {
          DebugLog(100, FMT_STRING("received nothing"));
          return bRC_Error;
        }

        return bRC_OK;
      }
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
  std::unique_ptr<bp::Plugin::Stub> stub_{};

  size_t current_xattr_index{std::numeric_limits<size_t>::max()};
  std::vector<bp::Xattribute> xattribute_cache{};
};

}  // namespace

// namespace {
struct grpc_connection_members {
  PluginClient client;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<grpc::Server> server;
  std::vector<std::unique_ptr<grpc::Service>> services;

  bool reading = false;
  bool writing = false;

  grpc_connection_members() = delete;
};


struct connection_builder {
  std::shared_ptr<grpc::Channel> channel{};
  std::optional<PluginClient> opt_client{};
  std::unique_ptr<grpc::Server> opt_server{};
  std::vector<std::unique_ptr<grpc::Service>> services{};

  template <typename... Args> connection_builder(Args&&... args)
  {
    (services.emplace_back(std::forward<Args>(args)), ...);
  }

  connection_builder& connect_client(Socket s)
  {
    channel = grpc::CreateInsecureChannelFromFd("", s.get());

    if (channel) {
      DebugLog(100, FMT_STRING("could connect to client over socket {}"),
               s.get());

      s.release();
      opt_client.emplace(channel);

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

      opt_server = builder.BuildAndStart();

      if (!opt_server) {
        DebugLog(50, FMT_STRING("grpc server could not get started"), s.get());
        return *this;
      }

      grpc::AddInsecureChannelFromFd(opt_server.get(), s.release());

    } catch (const std::exception& e) {
      DebugLog(50, FMT_STRING("could not attach socket {} to server: Err={}"),
               s.get(), e.what());
      opt_server.reset();
    } catch (...) {
      opt_server.reset();
    }

    return *this;
  }


  std::optional<grpc_connection> build()
  {
    if (!opt_client) { return std::nullopt; }
    if (!opt_server) { return std::nullopt; }

    grpc_connection con{};

    con.members = new grpc_connection_members{
        std::move(opt_client.value()), std::move(channel),
        std::move(opt_server), std::move(services)};

    return con;
  }
};
//};  // namespace

std::optional<grpc_connection> make_connection_from(PluginContext* ctx,
                                                    Socket in,
                                                    Socket out)
{
  return connection_builder{std::make_unique<BareosCore>(ctx)}
      .connect_client(std::move(out))
      .connect_server(std::move(in))
      .build();
}

bool SetNonBlocking(Socket& s)
{
  int flags = fcntl(s.get(), F_GETFL);
  if (flags == -1) {
    DebugLog(50, FMT_STRING("could not get flags for socket {}: Err={}"),
             s.get(), strerror(errno));
    return false;
  }

  if (fcntl(s.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
    DebugLog(
        50, FMT_STRING("could not add non blocking flags to socket {}: Err={}"),
        s.get(), strerror(errno));
    return false;
  }

  return true;
}

std::optional<grpc_child> make_connection(PluginContext* ctx,
                                          std::string_view program_path)
{
  // We want to create a two way grpc connection, where both ends act as both
  // a server and a client.  We do this by using two socket pairs.

  DebugLog(100, FMT_STRING("creating connection to {} ..."), program_path);

  auto to_program = unix_socket_pair();
  auto from_program = unix_socket_pair();
  auto io_socket = unix_socket_pair();

  if (!to_program || !from_program || !io_socket) {
    DebugLog(50,
             FMT_STRING("abort creation of connection to {} as socket pairs "
                        "could not be created"),
             program_path);
    return std::nullopt;
  }

  if (!SetNonBlocking(to_program->first) || !SetNonBlocking(to_program->second)
      || !SetNonBlocking(from_program->first)
      || !SetNonBlocking(from_program->second)) {
    return std::nullopt;
  }

  DebugLog(100, FMT_STRING("Created socket pairs {} <> {} | {} <> {}"),
           to_program->first.get(), to_program->second.get(),
           from_program->first.get(), from_program->second.get());

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
                               std::move(from_program->second),
                               std::move(io_socket->second));

  if (!child) {
    DebugLog(50,
             FMT_STRING("abort creation of connection to {} as program could "
                        "not get started"),
             program_path);
    return std::nullopt;
  }

  process p{*child};
  child.reset();

  auto con = make_connection_from(ctx, std::move(from_program->first),
                                  std::move(to_program->first));

  if (!con) {
    DebugLog(50, FMT_STRING("no connection for me :("));
    return std::nullopt;
  }

  DebugLog(100, FMT_STRING("a connection for me.  Finishing setup..."));

  if (con->Setup() == bRC_Error) {
    DebugLog(100, FMT_STRING("... unsuccessfully."));
    return std::nullopt;
  }

  DebugLog(100, FMT_STRING("... successfully."));

  return grpc_child{std::move(p), std::move(con.value()),
                    std::move(io_socket->first)};
}

process::~process()
{
  if (pid < 0) { return; }

  kill(pid, SIGKILL);

  // wait for the child to close (for now)
  for (;;) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
      DebugLog(50, FMT_STRING("wait pid failed. Err={}"), strerror(errno));
      break;
    } else {
      if (WIFEXITED(status)) {
        DebugLog(100, FMT_STRING("child exit status = {}"),
                 WEXITSTATUS(status));

        break;
      } else if (WIFSIGNALED(status)) {
        DebugLog(100, FMT_STRING("child signaled with {}"), WTERMSIG(status));
        break;
      } else {
        DebugLog(100, FMT_STRING("got status = {}"), status);
      }
    }
  }
}

grpc_connection::~grpc_connection() { delete members; }
bRC grpc_connection::handlePluginEvent(filedaemon::bEventType type, void* data)
{
  PluginClient* client = &members->client;
  return client->handlePluginEvent(type, data);
}
bRC grpc_connection::startBackupFile(filedaemon::save_pkt* pkt)
{
  PluginClient* client = &members->client;
  return client->startBackupFile(pkt);
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
bRC grpc_connection::pluginIO(filedaemon::io_pkt* pkt, int iosock)
{
  PluginClient* client = &members->client;
  switch (pkt->func) {
    case filedaemon::IO_OPEN: {
      return client->FileOpen(pkt->fname, pkt->flags, pkt->mode);
    } break;
    case filedaemon::IO_READ: {
      if (members->writing) {
        DebugLog(50, FMT_STRING("cannot read & write at the same time"));
        return bRC_Error;
      } else if (!members->reading) {
        // start reading
        size_t ignored;
        auto res = client->FileRead(pkt->buf, pkt->count, &ignored);
        if (res != bRC_OK) { return res; }
        members->reading = true;
      }

      int64_t count = 0;

      if (read(iosock, &count, sizeof(count)) < 0) {
        // this should not be happening
        DebugLog(50, FMT_STRING("could not read packet size. Err={}"),
                 strerror(errno));
        return bRC_Error;
      }

      if (count > pkt->count) {
        DebugLog(50, FMT_STRING("buffer to small"));
        return bRC_Error;
      }

      if (count > 0) {
        int64_t read_bytes = 0;
        while (read_bytes < count) {
          auto res = read(iosock, pkt->buf + read_bytes, count - read_bytes);
          if (res < 0) {
            DebugLog(50,
                     FMT_STRING("could not read data (pos = {}/{}). Err={}"),
                     read_bytes, count, strerror(errno));
            return bRC_Error;
          }

          read_bytes += res;
        }
      } else {
        members->reading = false;
      }

      pkt->status = count;
      return bRC_OK;
    } break;
    case filedaemon::IO_WRITE: {
      size_t count = 0;
      auto res = client->FileWrite(pkt->buf, pkt->count, &count);
      pkt->status = count;
      return res;
    } break;
    case filedaemon::IO_CLOSE: {
      members->reading = false;
      members->writing = false;
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
bRC grpc_connection::Setup()
{
  PluginClient* client = &members->client;
  return client->Setup();
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
