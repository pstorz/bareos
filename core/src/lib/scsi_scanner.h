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
 * @file scsi_scanner.h
 * SCSI device enumeration for tape drives and media changers.
 *
 * Scans /dev/sg* using SCSI INQUIRY to identify and classify all SCSI
 * sequential-access (tape) and medium-changer devices on the system.
 * Only compiled when HAVE_LOWLEVEL_SCSI_INTERFACE is defined.
 */

#ifndef BAREOS_LIB_SCSI_SCANNER_H_
#define BAREOS_LIB_SCSI_SCANNER_H_

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE

#  include <cstdint>
#  include <string>
#  include <vector>

namespace bareos::scsi {

// SCSI peripheral device type codes (SPC-5 §6.4.2)
constexpr uint8_t kScsiTypeTapeDrive = 0x01;  ///< Sequential-access device
constexpr uint8_t kScsiTypeChanger = 0x08;    ///< Medium changer

/** Identifies a single SCSI tape drive or medium changer on the host. */
struct ScsiDeviceInfo {
  std::string sg_path;   ///< e.g. /dev/sg2
  std::string st_path;   ///< e.g. /dev/nst0 (tape drives only; empty for
                         ///< changers)
  uint8_t device_type;   ///< kScsiTypeTapeDrive or kScsiTypeChanger
  std::string vendor;    ///< Trimmed vendor string  (≤8 chars)
  std::string product;   ///< Trimmed product string (≤16 chars)
  std::string revision;  ///< Trimmed firmware revision (≤4 chars)
  std::string serial;    ///< Serial number from VPD page 0x80 (may be empty)
};

/**
 * Scan /dev/sg* and return info for every tape drive and media changer found.
 *
 * Stops scanning when the next sg device does not exist.  Devices that are
 * neither tape drives nor changers are silently skipped.
 *
 * @return  Vector of ScsiDeviceInfo, one entry per discovered device.
 *          Returns empty vector if no sg devices exist or are accessible.
 */
std::vector<ScsiDeviceInfo> ScanScsiDevices();

/**
 * Trim trailing ASCII space characters from a fixed-width SCSI string field.
 *
 * SCSI INQUIRY returns space-padded strings (SPC-5 §4.4.1).  This function
 * copies the bytes, strips trailing spaces, and returns a std::string.
 * Exposed for unit testing.
 *
 * @param field  Pointer to the first byte of the SCSI field
 * @param len    Field length in bytes (e.g. 8 for vendor, 16 for product)
 * @return       Trimmed string (may be empty if all bytes are spaces)
 */
std::string TrimScsiString(const uint8_t* field, std::size_t len);

/**
 * Find the /dev/nstN device that corresponds to /dev/sgN via sysfs.
 *
 * Uses the kernel sysfs class hierarchy to locate the non-rewind tape device
 * node that shares the same SCSI address as the given generic SCSI device.
 * Returns an empty string if the device is not a tape drive or the mapping
 * cannot be determined.  Exposed for unit testing.
 *
 * @param sg_path  Path such as "/dev/sg2"
 * @return         Path such as "/dev/nst0", or empty string on failure
 */
std::string FindNstForSg(const std::string& sg_path);

}  // namespace bareos::scsi

#endif  // HAVE_LOWLEVEL_SCSI_INTERFACE
#endif  // BAREOS_LIB_SCSI_SCANNER_H_
