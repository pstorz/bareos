/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

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

#ifndef BAREOS_INCLUDE_DIR_SD_PROTOCOL_H_
#define BAREOS_INCLUDE_DIR_SD_PROTOCOL_H_

#include <string_view>

/* Version of the job protocol between director and storage daemon.
 * Increment it on every incompatible change; director and storage daemon
 * only run jobs together if both use the same version.
 *   1: FindMedia and GetVolInfo carry the device and drive index
 *   2: inventory command, director orders the devices to reserve */
inline constexpr int kDirSdProtocolVersion = 2;

enum class JobCommandCheck
{
  kOk,
  kVersionMismatch,
  kMalformed
};

/* Classify a parsed job command; fields is the bsscanf result, which
 * is -1 for a director that does not send SdProtocolVersion at all. */
constexpr JobCommandCheck CheckJobCommand(std::string_view cmd,
                                          int fields,
                                          int expected_fields,
                                          int version)
{
  if (cmd.find(" SdProtocolVersion=") == std::string_view::npos) {
    return JobCommandCheck::kVersionMismatch;
  }
  if (fields != expected_fields) { return JobCommandCheck::kMalformed; }
  return version == kDirSdProtocolVersion ? JobCommandCheck::kOk
                                          : JobCommandCheck::kVersionMismatch;
}

#endif  // BAREOS_INCLUDE_DIR_SD_PROTOCOL_H_
