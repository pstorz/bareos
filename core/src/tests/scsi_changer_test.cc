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
 * @file scsi_changer_test.cc
 * Unit tests for the native SCSI autochanger interface (scsi_changer.cc).
 *
 * Tests use hand-crafted READ ELEMENT STATUS response buffers so no
 * real hardware is needed.
 */

#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "gtest/gtest.h"
#else
#  include "gtest/gtest.h"
#  include "include/bareos.h"
#endif

#include <cstring>
#include <cstddef>

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE

#  include "lib/scsi_changer.h"

using namespace bareos::scsi;

// ── CDB construction tests ───────────────────────────────────────────────────

TEST(ScsiChangerCDB, MoveMediumOpcodeAndLayout)
{
  auto cdb = BuildMoveMediumCDB(0x0000, 0x0001, 0x0101);
  EXPECT_EQ(cdb.opcode, 0xA5u);
  EXPECT_EQ(cdb.transport_addr[0], 0x00u);
  EXPECT_EQ(cdb.transport_addr[1], 0x00u);
  EXPECT_EQ(cdb.source_addr[0], 0x00u);
  EXPECT_EQ(cdb.source_addr[1], 0x01u);
  EXPECT_EQ(cdb.dest_addr[0], 0x01u);
  EXPECT_EQ(cdb.dest_addr[1], 0x01u);
  EXPECT_EQ(cdb.invert, 0x00u);
  EXPECT_EQ(cdb.control, 0x00u);
}

static_assert(sizeof(MoveMediumCDB) == 12,
              "MoveMediumCDB must be exactly 12 bytes");
static_assert(sizeof(ReadElementStatusCDB) == 12,
              "ReadElementStatusCDB must be exactly 12 bytes");

// ── ParseElementStatusResponse tests ─────────────────────────────────────────

/**
 * Build a minimal READ ELEMENT STATUS response with:
 *   - 1 storage element  (address 0x0001, FULL, barcode "VOL001")
 *   - 1 storage element  (address 0x0002, empty)
 *   - 1 data transfer el (address 0x0101, loaded from slot 1)
 *
 * SMC-3 layout:
 *   [0..7]  response header
 *   [8..15] storage element page header
 *   [16..99] storage element descriptors (2 × 42 bytes with VolTag)
 *   [100..107] drive element page header
 *   [108..149] drive element descriptor (42 bytes with VolTag)
 *
 * We use a descriptor length of 42 bytes = 12 base bytes + 30 bytes VolTag
 * (barcode in first 12 bytes of VolTag for simplicity).
 */
namespace {

constexpr uint16_t kSlot1Addr = 0x0001;
constexpr uint16_t kSlot2Addr = 0x0002;
constexpr uint16_t kDriveAddr = 0x0101;
constexpr uint16_t kDescLen = 48; /* 12 base + 36 VolTag */
constexpr uint32_t kStoragePageLen
    = 2 * kDescLen; /* 2 storage element descriptors */
constexpr uint32_t kDrivePageLen = kDescLen;

/** Write a 2-byte big-endian value into buf at offset */
static void WB16(uint8_t* buf, std::size_t offset, uint16_t v)
{
  buf[offset] = static_cast<uint8_t>(v >> 8);
  buf[offset + 1] = static_cast<uint8_t>(v & 0xff);
}

/** Write a 3-byte big-endian value into buf at offset */
static void WB24(uint8_t* buf, std::size_t offset, uint32_t v)
{
  buf[offset] = static_cast<uint8_t>((v >> 16) & 0xff);
  buf[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
  buf[offset + 2] = static_cast<uint8_t>(v & 0xff);
}

/** Write NUL-padded barcode at 36-byte VolTag field */
static void WriteBarcode(uint8_t* buf, const char* bc)
{
  std::memset(buf, ' ', 36);
  std::size_t len = std::strlen(bc);
  if (len > 32) { len = 32; }
  std::memcpy(buf, bc, len);
}

std::vector<uint8_t> BuildTestResponse()
{
  // total page data = storage page (8 hdr + 2*48 desc) + drive page (8 hdr +
  // 1*48 desc)
  constexpr std::size_t kStoragePage = 8 + 2 * kDescLen;
  constexpr std::size_t kDrivePage = 8 + kDescLen;
  constexpr std::size_t kPageDataTotal = kStoragePage + kDrivePage;
  constexpr std::size_t kTotal = 8 + kPageDataTotal;

  std::vector<uint8_t> buf(kTotal, 0);

  // Response header (8 bytes)
  WB16(buf.data(), 0, kSlot1Addr); /* First element address reported */
  WB16(buf.data(), 2, 3);          /* num elements available (2 slots + 1 drive) */
  /* reserved byte 4 */
  WB24(buf.data(), 5, static_cast<uint32_t>(kPageDataTotal)); /* byte count */

  // ── Storage element page header (8 bytes at offset 8) ────────────────────
  std::size_t off = 8;
  buf[off + 0] = 0x02; /* element type: storage */
  buf[off + 1] = 0x80; /* PVolTag=1 */
  WB16(buf.data(), off + 2, kDescLen);
  /* reserved byte off+4 */
  WB24(buf.data(), off + 5, kStoragePageLen);
  off += 8;

  // Storage element 1 descriptor (slot 1, FULL, barcode "VOL001")
  WB16(buf.data(), off + 0, kSlot1Addr);
  buf[off + 2] = 0x01; /* FULL bit */
  /* bytes 3-9: reserved / sense */
  /* byte 9: SValid=0 (nothing to report for storage) */
  /* bytes 10-11: source addr (unused) */
  WriteBarcode(buf.data() + off + 12, "VOL001");
  off += kDescLen;

  // Storage element 2 descriptor (slot 2, empty)
  WB16(buf.data(), off + 0, kSlot2Addr);
  buf[off + 2] = 0x00; /* not FULL */
  off += kDescLen;

  // ── Drive element page header (8 bytes) ───────────────────────────────────
  buf[off + 0] = 0x04; /* element type: data transfer */
  buf[off + 1] = 0x80; /* PVolTag=1 */
  WB16(buf.data(), off + 2, kDescLen);
  WB24(buf.data(), off + 5, kDrivePageLen);
  off += 8;

  // Drive 0 descriptor: loaded with slot 1
  WB16(buf.data(), off + 0, kDriveAddr);
  buf[off + 2] = 0x01; /* FULL */
  buf[off + 9] = 0x80; /* SValid=1 */
  WB16(buf.data(), off + 10, kSlot1Addr); /* source = slot 1 */
  WriteBarcode(buf.data() + off + 12, "VOL001");

  return buf;
}

} /* anonymous namespace */

TEST(ScsiChangerParse, TwoSlotsOneDrive)
{
  auto buf = BuildTestResponse();
  ChangerInventory inv;

  ASSERT_TRUE(ParseElementStatusResponse(buf.data(), buf.size(), inv));

  ASSERT_EQ(inv.slots.size(), 2u);
  ASSERT_EQ(inv.drives.size(), 1u);
  EXPECT_EQ(inv.ie_slots.size(), 0u);

  // Slot 1
  EXPECT_EQ(inv.slots[0].address, kSlot1Addr);
  EXPECT_EQ(inv.slots[0].logical_slot, 1u);
  EXPECT_TRUE(inv.slots[0].full);
  EXPECT_STREQ(inv.slots[0].barcode, "VOL001");

  // Slot 2
  EXPECT_EQ(inv.slots[1].address, kSlot2Addr);
  EXPECT_EQ(inv.slots[1].logical_slot, 2u);
  EXPECT_FALSE(inv.slots[1].full);

  // Drive 0
  EXPECT_EQ(inv.drives[0].address, kDriveAddr);
  EXPECT_EQ(inv.drives[0].logical_drive, 0u);
  EXPECT_TRUE(inv.drives[0].loaded);
  EXPECT_EQ(inv.drives[0].loaded_from_address, kSlot1Addr);
  EXPECT_EQ(inv.drives[0].loaded_from_slot, 1u);
  EXPECT_STREQ(inv.drives[0].barcode, "VOL001");

  // Inventory summary fields
  EXPECT_EQ(inv.first_slot_address, kSlot1Addr);
  EXPECT_EQ(inv.first_drive_address, kDriveAddr);
  EXPECT_EQ(inv.num_slots, 2u);
  EXPECT_EQ(inv.num_drives, 1u);
}

TEST(ScsiChangerParse, EmptyBuffer)
{
  ChangerInventory inv;
  // Buffer too short: must fail gracefully
  uint8_t tiny[3] = {0};
  EXPECT_FALSE(ParseElementStatusResponse(tiny, sizeof(tiny), inv));
}

TEST(ScsiChangerParse, ZeroByteCount)
{
  // Valid 8-byte header with byte_count=0 → empty library, returns true
  uint8_t buf[8] = {0};
  ChangerInventory inv;
  EXPECT_TRUE(ParseElementStatusResponse(buf, sizeof(buf), inv));
  EXPECT_EQ(inv.slots.size(), 0u);
  EXPECT_EQ(inv.drives.size(), 0u);
}

TEST(ScsiChangerParse, DriveUnloaded)
{
  auto buf = BuildTestResponse();

  // Flip slot 1 to empty and drive to unloaded
  constexpr std::size_t kSlot1DescOff = 8 + 8; /* resp hdr + page hdr */
  buf[kSlot1DescOff + 2] = 0x00;               /* clear FULL on slot 1 */

  constexpr std::size_t kDriveDescOff
      = 8 + 8 + 2 * kDescLen + 8; /* after storage page */
  buf[kDriveDescOff + 2] = 0x00; /* clear FULL on drive */
  buf[kDriveDescOff + 9] = 0x00; /* clear SValid */

  ChangerInventory inv;
  ASSERT_TRUE(ParseElementStatusResponse(buf.data(), buf.size(), inv));

  EXPECT_FALSE(inv.slots[0].full);
  EXPECT_FALSE(inv.drives[0].loaded);
  EXPECT_EQ(inv.drives[0].loaded_from_slot, 0u);
}

#endif /* HAVE_LOWLEVEL_SCSI_INTERFACE */
