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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

auto PluginService::handlePluginEvent(
    ServerContext*,
    const bp::handlePluginEventRequest* request,
    bp::handlePluginEventResponse* response) -> Status
{
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
