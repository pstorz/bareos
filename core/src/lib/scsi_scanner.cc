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
 * @file scsi_scanner.cc
 * SCSI device enumeration for tape drives and media changers.
 *
 * Scans /dev/sg* using SCSI INQUIRY (opcode 0x12) and classifies each
 * device as a sequential-access (tape) device or a medium changer.
 */

#include "include/bareos.h"

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE

#  include "lib/scsi_scanner.h"
#  include "lib/scsi_lli.h"
#  include "lib/berrno.h"

#  include <cerrno>
#  include <cstring>
#  include <cstdio>
#  include <dirent.h>
#  include <fcntl.h>
#  include <unistd.h>

namespace bareos::scsi {

// ── SCSI INQUIRY CDB ─────────────────────────────────────────────────────────

struct InquiryCDB {
  uint8_t opcode;       /**< 0x12 */
  uint8_t evpd;         /**< bit0: enable vital product data */
  uint8_t page_code;    /**< VPD page code (valid when evpd=1) */
  uint8_t reserved;
  uint8_t alloc_length; /**< allocation length */
  uint8_t control;      /**< 0x00 */
};
static_assert(sizeof(InquiryCDB) == 6, "InquiryCDB must be 6 bytes");

// ── TrimScsiString ────────────────────────────────────────────────────────────

std::string TrimScsiString(const uint8_t* field, std::size_t len)
{
  if (len == 0) { return {}; }
  std::string s(reinterpret_cast<const char*>(field), len);
  std::size_t end = s.find_last_not_of(' ');
  if (end == std::string::npos) { return {}; }
  return s.substr(0, end + 1);
}

// ── FindNstForSg ──────────────────────────────────────────────────────────────

std::string FindNstForSg(const std::string& sg_path)
{
  // Extract device name: "/dev/sg2" -> "sg2"
  std::size_t slash = sg_path.rfind('/');
  std::string sg_name
      = (slash == std::string::npos) ? sg_path : sg_path.substr(slash + 1);

  // Check /sys/class/scsi_generic/<sg_name>/device/scsi_tape/
  std::string tape_dir = "/sys/class/scsi_generic/" + sg_name
                         + "/device/scsi_tape/";

  DIR* dir = opendir(tape_dir.c_str());
  if (!dir) { return {}; }

  std::string result;
  struct dirent* entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    const char* name = entry->d_name;
    // Skip . and ..
    if (name[0] == '.') { continue; }
    // Look for nstN where N is all digits
    if (name[0] == 'n' && name[1] == 's' && name[2] == 't') {
      bool all_digits = true;
      for (const char* p = name + 3; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
          all_digits = false;
          break;
        }
      }
      if (all_digits && name[3] != '\0') {
        result = std::string("/dev/") + name;
        break;
      }
    }
  }
  closedir(dir);
  return result;
}

// ── ScanScsiDevices ───────────────────────────────────────────────────────────

std::vector<ScsiDeviceInfo> ScanScsiDevices()
{
  std::vector<ScsiDeviceInfo> devices;

  for (int i = 0;; ++i) {
    std::string path = "/dev/sg" + std::to_string(i);

    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_BINARY);
    if (fd < 0) {
      if (errno == ENOENT) { break; }  // no more sg devices
      continue;                        // permission denied or similar — skip
    }

    // Issue standard INQUIRY (evpd=0)
    constexpr unsigned kInquiryLen = 96;
    uint8_t buf[kInquiryLen] = {};
    InquiryCDB cdb{0x12, 0, 0, 0, static_cast<uint8_t>(kInquiryLen), 0};
    bool ok = RecvScsiCmdPage(fd, path.c_str(), &cdb, sizeof(cdb), buf,
                              kInquiryLen);
    close(fd);

    if (!ok) { continue; }

    uint8_t device_type = buf[0] & 0x1Fu;
    if (device_type != kScsiTypeTapeDrive && device_type != kScsiTypeChanger) {
      continue;
    }

    ScsiDeviceInfo info{};
    info.sg_path = path;
    info.device_type = device_type;
    info.vendor = TrimScsiString(&buf[8], 8);
    info.product = TrimScsiString(&buf[16], 16);
    info.revision = TrimScsiString(&buf[32], 4);

    if (device_type == kScsiTypeTapeDrive) {
      info.st_path = FindNstForSg(path);
    }

    // VPD page 0x80 — unit serial number (fd=-1: RecvScsiCmdPage opens device)
    constexpr unsigned kVpdLen = 64;
    uint8_t vpd[kVpdLen] = {};
    InquiryCDB vpd_cdb{0x12, 1, 0x80, 0, static_cast<uint8_t>(kVpdLen), 0};
    if (RecvScsiCmdPage(-1, path.c_str(), &vpd_cdb, sizeof(vpd_cdb), vpd,
                        kVpdLen)) {
      uint8_t page_len = vpd[3];
      if (page_len > 0 && page_len <= kVpdLen - 4u) {
        info.serial = TrimScsiString(&vpd[4], page_len);
      }
    }
    // If VPD fails, serial stays empty — non-fatal

    devices.push_back(std::move(info));
  }

  return devices;
}

}  // namespace bareos::scsi

#endif  // HAVE_LOWLEVEL_SCSI_INTERFACE
