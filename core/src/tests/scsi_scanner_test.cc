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
 * @file scsi_scanner_test.cc
 * Unit tests for SCSI device scanner helpers (scsi_scanner.cc).
 *
 * Tests use pure helper functions so no real SCSI hardware is required.
 */

#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "gtest/gtest.h"
#else
#  include "gtest/gtest.h"
#  include "include/bareos.h"
#endif

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE

#  include "lib/scsi_scanner.h"

using namespace bareos::scsi;

// ── TrimScsiString tests ─────────────────────────────────────────────────────

TEST(ScsiScannerTest, TrimTrailingSpaces)
{
  const uint8_t field[] = {'V', 'E', 'N', 'D', 'O', 'R', ' ', ' '};
  EXPECT_EQ(TrimScsiString(field, sizeof(field)), "VENDOR");
}

TEST(ScsiScannerTest, TrimAllSpaces)
{
  const uint8_t field[] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  EXPECT_EQ(TrimScsiString(field, sizeof(field)), "");
}

TEST(ScsiScannerTest, TrimNoTrailing)
{
  const uint8_t field[] = {'V', 'E', 'N', 'D', 'O', 'R'};
  EXPECT_EQ(TrimScsiString(field, sizeof(field)), "VENDOR");
}

TEST(ScsiScannerTest, TrimEmptyField)
{
  EXPECT_EQ(TrimScsiString(nullptr, 0), "");
}

// ── FindNstForSg tests ────────────────────────────────────────────────────────

TEST(ScsiScannerTest, FindNstNonExistentSg)
{
  // /dev/sg999 will not have a sysfs entry — must return empty string
  EXPECT_EQ(FindNstForSg("/dev/sg999"), "");
}

// ── ScanScsiDevices tests ─────────────────────────────────────────────────────

TEST(ScsiScannerTest, ScanNoDevices)
{
  // In CI there are no real SCSI devices; the function must not crash and
  // must return a (possibly empty) vector.
  std::vector<ScsiDeviceInfo> devs = ScanScsiDevices();
  // No assertion on content — just verify it returns without throwing.
  SUCCEED();
}

#endif  // HAVE_LOWLEVEL_SCSI_INTERFACE
