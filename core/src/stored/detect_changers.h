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

/**
 * @file detect_changers.h
 * Storage Daemon "detect changers" director command.
 */

#ifndef BAREOS_STORED_DETECT_CHANGERS_H_
#define BAREOS_STORED_DETECT_CHANGERS_H_

#include "include/jcr.h"

namespace storagedaemon {

/**
 * Handle the "detect changers" command from the Director.
 *
 * Scans for SCSI medium changers, reads their element inventory,
 * and sends structured response lines to the Director socket.
 *
 * @return true on success (even if no changers found),
 *         false only on socket/protocol error.
 */
bool DetectChangersCmd(JobControlRecord* jcr);

}  // namespace storagedaemon

#endif  // BAREOS_STORED_DETECT_CHANGERS_H_
