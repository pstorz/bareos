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
 * @file detect_changers.cc
 * Storage Daemon "detect changers" director command implementation.
 *
 * Scans the host for SCSI medium changers via ScanScsiDevices(), issues
 * READ ELEMENT STATUS on each, and reports the full inventory to the
 * Director as structured response lines (3000..3099).
 */

#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "include/jcr.h"
#include "lib/bsock.h"
#include "stored/detect_changers.h"

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
#  include "lib/scsi_scanner.h"
#  include "lib/scsi_changer.h"
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace storagedaemon {

bool DetectChangersCmd(JobControlRecord* jcr)
{
  BareosSocket* dir = jcr->dir_bsock;

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  using namespace bareos::scsi;

  std::vector<ScsiDeviceInfo> devices = ScanScsiDevices();

  for (const ScsiDeviceInfo& info : devices) {
    if (info.device_type != kScsiTypeChanger) { continue; }

    int fd = open(info.sg_path.c_str(), O_RDWR | O_NONBLOCK | O_BINARY);
    if (fd < 0) { continue; }

    ChangerInventory inv{};
    bool ok = ReadElementStatus(fd, info.sg_path.c_str(), inv);
    close(fd);

    if (!ok) { continue; }

    // 3000: changer summary line
    if (!dir->fsend(
            "3000 changer sg=%s vendor=%s product=%s serial=%s"
            " slots=%u drives=%u\n",
            info.sg_path.c_str(), info.vendor.c_str(), info.product.c_str(),
            info.serial.c_str(), static_cast<unsigned>(inv.num_slots),
            static_cast<unsigned>(inv.num_drives))) {
      return false;
    }

    // 3001: one line per drive
    for (const DriveInfo& drv : inv.drives) {
      // Find matching sg scanner device for this drive index if possible.
      // We report the changer sg_path as the drive sg path since the
      // individual drive sg paths require separate enumeration.
      if (!dir->fsend("3001 drive index=%u sg=%s st=%s\n",
                      static_cast<unsigned>(drv.logical_drive),
                      info.sg_path.c_str(), info.st_path.c_str())) {
        return false;
      }
    }

    // 3002: one line per storage slot
    for (const SlotInfo& slot : inv.slots) {
      if (!dir->fsend("3002 slot num=%u full=%d barcode=%s\n",
                      static_cast<unsigned>(slot.logical_slot),
                      static_cast<int>(slot.full), slot.barcode)) {
        return false;
      }
    }
  }

#endif  // HAVE_LOWLEVEL_SCSI_INTERFACE

  return dir->fsend("3099 OK detect changers\n");
}

}  // namespace storagedaemon
