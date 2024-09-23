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
      /* TODO: how best to report an error here ? */
      __builtin_unreachable();
  }
}


std::optional<grpc_connection> make_connection(std::string_view program_path)
{
  // We want to create a two way grpc connection, where both ends act as both
  // a server and a client.  We do this by using two socket pairs.

  int to_program[2] = {};    // we are the client here
  int from_program[2] = {};  // we are the server here

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, to_program) < 0) {
    return std::nullopt;
  }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, from_program) < 0) {
    return std::nullopt;
  }

  (void)program_path;
  return std::nullopt;
}
