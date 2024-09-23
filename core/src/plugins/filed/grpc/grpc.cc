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

#include <cstdio>
#include <thread>
#include "grpc_impl.h"

#include "include/bareos.h"
#include "filed/fd_plugins.h"
#include "sys/types.h"
#include "sys/socket.h"

template <typename... Args> void ignore(Args&&...) {}

#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>

#define debug(funcs, num, fmt, ...)                         \
  (funcs)->DebugMessage(nullptr, __FILE__, __LINE__, (num), \
                        (fmt)__VA_OPT__(, ) __VA_ARGS__)

bool send_fd(int socket, int fd)
{
  struct msghdr write_msg = {};

  alignas(struct cmsghdr) char buffer[CMSG_SPACE(sizeof(int))];

  struct iovec dummy = {.iov_base = (char*)"", .iov_len = 1};

  write_msg.msg_control = buffer;
  write_msg.msg_controllen = sizeof(buffer);  // set to capacity
  write_msg.msg_iov = &dummy;
  write_msg.msg_iovlen = 1;

  auto* cmsg = CMSG_FIRSTHDR(&write_msg);

  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));

  memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

  write_msg.msg_controllen = cmsg->cmsg_len;  // set to actual size
  if (sendmsg(socket, &write_msg, 0) < 0) { return false; }

  return true;
}

std::optional<int> receive_fd(filedaemon::CoreFunctions* funcs, int socket)
{
  struct msghdr read_msg = {};

  alignas(struct cmsghdr) char buffer[CMSG_SPACE(sizeof(int))];

  read_msg.msg_control = buffer;
  read_msg.msg_controllen = sizeof(buffer);

  auto rc = recvmsg(socket, &read_msg, 0);

  debug(funcs, 100, "rc = %lld\n", rc);

  if (rc < 0) { return std::nullopt; }

  debug(funcs, 100, "clen = %lld\n", read_msg.msg_controllen);

  if (read_msg.msg_controllen <= 0) { return std::nullopt; }

  auto* hdr = CMSG_FIRSTHDR(&read_msg);
  if (!hdr) {
    return std::nullopt;
    ;
  }

  debug(funcs, 100, "hdr = %p, len = %llu\n", hdr, hdr->cmsg_len);

  if (hdr->cmsg_len != CMSG_LEN(sizeof(int))) { return std::nullopt; }

  auto* data = CMSG_DATA(hdr);

  int fd{-1};

  memcpy(&fd, data, sizeof(int));

  return fd;
}

inline bool TestSocket(filedaemon::CoreFunctions* funcs)
{
  int sockets[2] = {};

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) { return false; }

  auto& child_sock = sockets[1];
  auto& parent_sock = sockets[0];

  char message[] = "Hallo";

  pid_t child = fork();
  if (child < 0) {
    close(child_sock);
    close(parent_sock);
    return false;
  } else if (child == 0) {
    close(parent_sock);

    int fd = memfd_create("test_sockets", 0);

    if (fd < 0) { return false; }

    if (write(fd, message, sizeof(message) - 1) != 5) { return false; }

    if (!send_fd(child_sock, fd)) { exit(1); }

    exit(0);
  } else {
    close(child_sock);

    auto opt = receive_fd(funcs, parent_sock);

    bool ok = [&]() {
      if (!opt) {
        debug(funcs, 100, "no fd\n");
        return false;
      }

      int fd = *opt;
      debug(funcs, 100, "fd = %d\n", fd);

      char fdbuf[6];

      lseek(fd, 0, SEEK_SET);

      if (auto res = read(fd, fdbuf, sizeof(fdbuf) - 1); res < 5) {
        debug(funcs, 100, "bad read (%lld)\n", res);
        return false;
      }

      fdbuf[5] = 0;
      debug(funcs, 100, "received: %s\n", fdbuf);

      if (memcmp(fdbuf, message, sizeof(message) - 1) != 0) {
        debug(funcs, 100, "bad data received\n");
        return false;
      }

      close(fd);
      return true;
    }();

    close(parent_sock);

    for (;;) {
      int status{};
      waitpid(child, &status, 0);

      if (WIFEXITED(status)) {
        auto retval = WEXITSTATUS(status);
        debug(funcs, 100, "retval = %d\n", retval);
        if (retval != 0) { return false; }
        break;
      }
    }

    return ok;
  }
}


namespace {
[[maybe_unused]] bool OpenConnection()
{
  int grpc_socket[2] = {};  // used for sett
  int meta_socket[2] = {};

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, grpc_socket) < 0) { return false; }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, meta_socket) < 0) { return false; }

  return 0;
}

bRC newPlugin(PluginContext*) { return bRC_Error; }
bRC freePlugin(PluginContext*) { return bRC_Error; }
bRC getPluginValue(PluginContext*, filedaemon::pVariable, void*)
{
  return bRC_Error;
}
bRC setPluginValue(PluginContext*, filedaemon::pVariable, void*)
{
  return bRC_Error;
}
bRC handlePluginEvent(PluginContext*, filedaemon::bEvent*, void*)
{
  return bRC_Error;
}
bRC startBackupFile(PluginContext*, filedaemon::save_pkt*) { return bRC_Error; }
bRC endBackupFile(PluginContext*) { return bRC_Error; }
bRC startRestoreFile(PluginContext*, const char*) { return bRC_Error; }
bRC endRestoreFile(PluginContext*) { return bRC_Error; }
bRC pluginIO(PluginContext*, filedaemon::io_pkt*) { return bRC_Error; }
bRC createFile(PluginContext*, filedaemon::restore_pkt*) { return bRC_Error; }
bRC setFileAttributes(PluginContext*, filedaemon::restore_pkt*)
{
  return bRC_Error;
}
bRC checkFile(PluginContext*, char*) { return bRC_Error; }
bRC getAcl(PluginContext*, filedaemon::acl_pkt*) { return bRC_Error; }
bRC setAcl(PluginContext*, filedaemon::acl_pkt*) { return bRC_Error; }
bRC getXattr(PluginContext*, filedaemon::xattr_pkt*) { return bRC_Error; }
bRC setXattr(PluginContext*, filedaemon::xattr_pkt*) { return bRC_Error; }
}  // namespace

constexpr PluginInformation my_info = {
    .size = sizeof(my_info),
    .version = FD_PLUGIN_INTERFACE_VERSION,
    .plugin_magic = FD_PLUGIN_MAGIC,
    .plugin_license = "Bareos AGPLv3",
    .plugin_author = "Sebastian Sura",
    .plugin_date = "September 2024",
    .plugin_version = "0.0.1",
    .plugin_description = "a simple grpc plugin",
    .plugin_usage = "Not sure yet",
};

constexpr filedaemon::PluginFunctions my_functions = {
    .size = sizeof(my_functions),
    .version = FD_PLUGIN_INTERFACE_VERSION,
    .newPlugin = &newPlugin,
    .freePlugin = &freePlugin,
    .getPluginValue = &getPluginValue,
    .setPluginValue = &setPluginValue,
    .handlePluginEvent = &handlePluginEvent,
    .startBackupFile = &startBackupFile,
    .endBackupFile = &endBackupFile,
    .startRestoreFile = &startRestoreFile,
    .endRestoreFile = &endRestoreFile,
    .pluginIO = &pluginIO,
    .createFile = &createFile,
    .setFileAttributes = &setFileAttributes,
    .checkFile = &checkFile,
    .getAcl = &getAcl,
    .setAcl = &setAcl,
    .getXattr = &getXattr,
    .setXattr = &setXattr,
};

bool AmICompatibleWith(filedaemon::PluginApiDefinition* core_info)
{
  if (core_info->size != sizeof(*core_info)
      || core_info->version != FD_PLUGIN_INTERFACE_VERSION) {
    return false;
  }

  return true;
}

extern "C" int loadPlugin(filedaemon::PluginApiDefinition* core_info,
                          filedaemon::CoreFunctions* core_funcs,
                          PluginInformation** plugin_info,
                          filedaemon::PluginFunctions** plugin_funcs)
{
  if (!AmICompatibleWith(core_info)) { return -1; }

  *plugin_info = const_cast<PluginInformation*>(&my_info);
  *plugin_funcs = const_cast<filedaemon::PluginFunctions*>(&my_functions);

  ignore(core_funcs);

  core_funcs->DebugMessage(nullptr, __FILE__, __LINE__, 100, "Hallo\n");

  if (!TestSocket(core_funcs)) {
    core_funcs->DebugMessage(nullptr, __FILE__, __LINE__, 100, "Bad\n");
    return -1;
  }

  core_funcs->DebugMessage(nullptr, __FILE__, __LINE__, 100, "Good\n");
  return 0;
}

extern "C" int unloadPlugin() { return 0; }
