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

#include "plugins/filed/grpc/grpc_impl.h"
#include <fcntl.h>
#include <thread>
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

  return std::nullopt;
}
