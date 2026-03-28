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
 * @file scsi_changer.cc
 * Native SCSI media-changer interface (SCSI SMC-3).
 *
 * Implements READ ELEMENT STATUS (0xB8) parsing and MOVE MEDIUM (0xA5)
 * to replace the mtx-changer shell script when UseNativeScsi = yes.
 */

#include "include/bareos.h"

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE

#  include "lib/scsi_changer.h"
#  include "lib/berrno.h"

#  include <algorithm>
#  include <cerrno>
#  include <cstring>
#  include <unistd.h>
#  include <sys/ioctl.h>

#  if defined(HAVE_LINUX_OS)
#    include <scsi/sg.h>
#    include <scsi/scsi.h>
#  endif

namespace bareos::scsi {

// ── internal helpers ─────────────────────────────────────────────────────────

static inline uint16_t be16(const uint8_t* p)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static inline uint32_t be24(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 16)
         | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

static inline uint32_t be32(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

static inline void put_be16(uint8_t* p, uint16_t v)
{
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xff);
}

static inline void put_be24(uint8_t* p, uint32_t v)
{
  p[0] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>(v & 0xff);
}

/** Extract a NUL-terminated barcode from a 36-byte volume tag field. */
static void ExtractBarcode(const uint8_t* voltag_field, char* out, int out_len)
{
  // First 32 bytes are the ASCII volume identifier, space-padded on the right
  int len = std::min(out_len - 1, 32);
  int last = 0;
  for (int i = 0; i < len; ++i) {
    char c = static_cast<char>(voltag_field[i]);
    out[i] = c;
    if (c != ' ' && c != '\0') { last = i + 1; }
  }
  out[last] = '\0';
}

// ── SG_IO helpers ────────────────────────────────────────────────────────────

#  if defined(HAVE_LINUX_OS)

struct SenseData {
  uint8_t response_code : 7;
  uint8_t valid : 1;
  uint8_t obsolete;
  uint8_t sense_key : 4;
  uint8_t res : 4;
  uint8_t information[4];
  uint8_t additional_sense_len;
  uint8_t cmd_specific[4];
  uint8_t asc;   /**< Additional Sense Code */
  uint8_t ascq;  /**< Additional Sense Code Qualifier */
  uint8_t fruc;
  uint8_t sks[3];
  uint8_t additional[109];
};

/**
 * Issue a SCSI command with no data-transfer phase (e.g. MOVE MEDIUM).
 * On CHECK CONDITION with sense key 0x05 / ASC 0x3B / ASCQ 0x0E
 * ("Medium Source Element Empty"), sets errno = ENOMEDIUM and returns false.
 */
static bool IssueScsiNoData(int fd,
                             const char* device_name,
                             void* cdb,
                             unsigned int cdb_len,
                             SenseData& sense)
{
  sg_io_hdr_t io_hdr;
  memset(&io_hdr, 0, sizeof(io_hdr));
  memset(&sense, 0, sizeof(sense));

  io_hdr.interface_id = 'S';
  io_hdr.cmd_len = cdb_len;
  io_hdr.mx_sb_len = sizeof(sense);
  io_hdr.dxfer_direction = SG_DXFER_NONE;
  io_hdr.dxfer_len = 0;
  io_hdr.dxferp = nullptr;
  io_hdr.cmdp = static_cast<unsigned char*>(cdb);
  io_hdr.sbp = reinterpret_cast<unsigned char*>(&sense);
  io_hdr.timeout = 120000; /* 120 s – changer moves can be slow */

  if (ioctl(fd, SG_IO, &io_hdr) < 0) {
    BErrNo be;
    Emsg2(M_ERROR, 0, T_("SG_IO ioctl on %s failed: ERR=%s\n"), device_name,
          be.bstrerror());
    return false;
  }

  if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
    // CHECK CONDITION — inspect sense data
    if (sense.sense_key == 0x05   /* ILLEGAL REQUEST */
        && sense.asc == 0x3B      /* MEDIUM DESTINATION ELEMENT FULL or ... */
        && sense.ascq == 0x0E) {  /* MEDIUM SOURCE ELEMENT EMPTY */
      errno = ENOMEDIUM;
    } else {
      Emsg4(M_ERROR, 0,
            T_("SCSI command on %s failed: sense_key=0x%02x "
               "asc=0x%02x ascq=0x%02x\n"),
            device_name, sense.sense_key, sense.asc, sense.ascq);
    }
    return false;
  }

  return true;
}

/** Issue a SCSI command that transfers data from device to host. */
static bool IssueScsiReadData(int fd,
                               const char* device_name,
                               void* cdb,
                               unsigned int cdb_len,
                               void* buf,
                               unsigned int buf_len)
{
  sg_io_hdr_t io_hdr;
  SenseData sense;
  memset(&io_hdr, 0, sizeof(io_hdr));
  memset(&sense, 0, sizeof(sense));

  io_hdr.interface_id = 'S';
  io_hdr.cmd_len = cdb_len;
  io_hdr.mx_sb_len = sizeof(sense);
  io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
  io_hdr.dxfer_len = buf_len;
  io_hdr.dxferp = static_cast<char*>(buf);
  io_hdr.cmdp = static_cast<unsigned char*>(cdb);
  io_hdr.sbp = reinterpret_cast<unsigned char*>(&sense);
  io_hdr.timeout = 60000; /* 60 s */

  if (ioctl(fd, SG_IO, &io_hdr) < 0) {
    BErrNo be;
    Emsg2(M_ERROR, 0, T_("SG_IO ioctl on %s failed: ERR=%s\n"), device_name,
          be.bstrerror());
    return false;
  }

  if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
    Emsg4(M_ERROR, 0,
          T_("SCSI read on %s failed: sense_key=0x%02x "
             "asc=0x%02x ascq=0x%02x\n"),
          device_name, sense.sense_key, sense.asc, sense.ascq);
    return false;
  }

  return true;
}

#  endif  /* HAVE_LINUX_OS */

// ── Public API implementations ───────────────────────────────────────────────

MoveMediumCDB BuildMoveMediumCDB(uint16_t transport_addr,
                                 uint16_t src_addr,
                                 uint16_t dst_addr)
{
  MoveMediumCDB cdb{};
  cdb.opcode = 0xA5;
  put_be16(cdb.transport_addr, transport_addr);
  put_be16(cdb.source_addr, src_addr);
  put_be16(cdb.dest_addr, dst_addr);
  return cdb;
}

bool ParseElementStatusResponse(const uint8_t* buf,
                                std::size_t len,
                                ChangerInventory& inv)
{
  if (len < 8) {
    Emsg0(M_ERROR, 0, T_("READ ELEMENT STATUS response too short\n"));
    return false;
  }

  // 8-byte response header
  uint32_t byte_count = be24(buf + 5); /* total bytes following header */

  if (byte_count == 0) {
    // No elements reported — not an error, just an empty library
    return true;
  }

  if (len < static_cast<std::size_t>(8 + byte_count)) {
    Emsg2(M_ERROR, 0,
          T_("READ ELEMENT STATUS buffer truncated: have %zu, need %u\n"), len,
          8 + byte_count);
    return false;
  }

  // Track lowest address seen per type to compute logical numbers
  bool slot_addr_set = false;
  bool drive_addr_set = false;
  bool ie_addr_set = false;

  const uint8_t* p = buf + 8;
  const uint8_t* end = buf + 8 + byte_count;

  while (p < end) {
    if (p + 8 > end) { break; } /* page header incomplete */

    uint8_t elem_type = p[0];
    bool pvoltag = (p[1] & 0x80) != 0;
    uint16_t descr_len = be16(p + 2);
    uint32_t page_byte_count = be24(p + 5);
    p += 8; /* skip page header */

    if (descr_len == 0 || page_byte_count == 0) { continue; }

    const uint8_t* page_end
        = p + page_byte_count; /* end of this page's descriptors */
    if (page_end > end) { page_end = end; }

    while (p + descr_len <= page_end) {
      uint16_t elem_addr = be16(p + 0);
      uint8_t flags = p[2];
      bool full = (flags & 0x01) != 0;
      bool svalid = (p[9] & 0x80) != 0;
      uint16_t src_addr = svalid ? be16(p + 10) : 0;

      char barcode[37] = {};
      if (pvoltag && descr_len >= 48) { ExtractBarcode(p + 12, barcode, 37); }

      switch (static_cast<ElementType>(elem_type)) {
        case ElementType::kStorage: {
          if (!slot_addr_set) {
            inv.first_slot_address = elem_addr;
            slot_addr_set = true;
          }
          SlotInfo si{};
          si.address = elem_addr;
          si.logical_slot = static_cast<uint16_t>(
              elem_addr - inv.first_slot_address + 1);
          si.full = full;
          si.is_import_export = false;
          std::memcpy(si.barcode, barcode, sizeof(si.barcode));
          inv.slots.push_back(si);
          break;
        }
        case ElementType::kImportExport: {
          if (!ie_addr_set) {
            inv.first_ie_address = elem_addr;
            ie_addr_set = true;
          }
          SlotInfo si{};
          si.address = elem_addr;
          si.logical_slot = static_cast<uint16_t>(
              elem_addr - inv.first_ie_address + 1);
          si.full = full;
          si.is_import_export = true;
          std::memcpy(si.barcode, barcode, sizeof(si.barcode));
          inv.ie_slots.push_back(si);
          break;
        }
        case ElementType::kDataTransfer: {
          if (!drive_addr_set) {
            inv.first_drive_address = elem_addr;
            drive_addr_set = true;
          }
          DriveInfo di{};
          di.address = elem_addr;
          di.logical_drive = static_cast<uint16_t>(
              elem_addr - inv.first_drive_address);
          di.loaded = full;
          di.loaded_from_address = src_addr;
          // Translate SCSI source address to logical slot number
          if (full && svalid && slot_addr_set
              && src_addr >= inv.first_slot_address) {
            di.loaded_from_slot = static_cast<uint16_t>(
                src_addr - inv.first_slot_address + 1);
          }
          std::memcpy(di.barcode, barcode, sizeof(di.barcode));
          inv.drives.push_back(di);
          break;
        }
        default:
          break; /* medium transport and unknown types ignored */
      }

      p += descr_len;
    }
    p = page_end; /* advance to next page even if stride didn't reach it */
  }

  inv.num_slots = static_cast<uint16_t>(inv.slots.size());
  inv.num_drives = static_cast<uint16_t>(inv.drives.size());
  inv.num_ie_slots = static_cast<uint16_t>(inv.ie_slots.size());

  return true;
}

bool ReadElementStatus(int fd,
                       const char* device_name,
                       ChangerInventory& inv,
                       bool request_volume_tags)
{
#  if defined(HAVE_LINUX_OS)
  constexpr uint32_t kBufSize = 65535;
  std::vector<uint8_t> buf(kBufSize, 0);

  ReadElementStatusCDB cdb{};
  cdb.opcode = 0xB8;
  // Request all element types; set VolTag bit if barcodes are wanted
  cdb.element_type_voltag
      = static_cast<uint8_t>(request_volume_tags ? 0x10 : 0x00);
  put_be16(cdb.start_element, 0x0000); /* start from address 0 */
  // Request up to 0xFFFF elements (all)
  put_be16(cdb.num_elements, 0xFFFF);
  put_be24(cdb.alloc_length, kBufSize);

  if (!IssueScsiReadData(fd, device_name, &cdb, sizeof(cdb), buf.data(),
                         kBufSize)) {
    return false;
  }

  return ParseElementStatusResponse(buf.data(), kBufSize, inv);
#  else
  Emsg0(M_ERROR, 0,
        T_("ReadElementStatus: native SCSI not supported on this platform\n"));
  return false;
#  endif
}

bool MoveMedium(int fd,
                const char* device_name,
                uint16_t src_addr,
                uint16_t dst_addr)
{
#  if defined(HAVE_LINUX_OS)
  MoveMediumCDB cdb = BuildMoveMediumCDB(0x0000, src_addr, dst_addr);
  SenseData sense{};
  return IssueScsiNoData(fd, device_name, &cdb, sizeof(cdb), sense);
#  else
  Emsg0(M_ERROR, 0,
        T_("MoveMedium: native SCSI not supported on this platform\n"));
  return false;
#  endif
}

bool InitializeElementStatus(int fd, const char* device_name)
{
#  if defined(HAVE_LINUX_OS)
  // INITIALIZE ELEMENT STATUS (opcode 0x07), 6-byte CDB
  uint8_t cdb[6] = {0x07, 0x00, 0x00, 0x00, 0x00, 0x00};
  SenseData sense{};
  return IssueScsiNoData(fd, device_name, cdb, sizeof(cdb), sense);
#  else
  Emsg0(M_ERROR, 0,
        T_("InitializeElementStatus: native SCSI not supported on this "
           "platform\n"));
  return false;
#  endif
}

} /* namespace bareos::scsi */

#endif /* HAVE_LOWLEVEL_SCSI_INTERFACE */
