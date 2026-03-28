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
 * @file scsi_changer.h
 * Native SCSI media-changer interface (SCSI SMC-3).
 *
 * Provides a pure-C++ replacement for the mtx-changer shell script.
 * Only compiled when HAVE_LOWLEVEL_SCSI_INTERFACE is defined (Linux/SG_IO).
 */

#ifndef BAREOS_LIB_SCSI_CHANGER_H_
#define BAREOS_LIB_SCSI_CHANGER_H_

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE

#  include <cstdint>
#  include <string>
#  include <vector>

namespace bareos::scsi {

// ── SCSI element type codes (SMC-3 §6.11.2) ─────────────────────────────────

enum class ElementType : uint8_t {
  kAll = 0,
  kMediumTransport = 1, /**< Robot arm / picker */
  kStorage = 2,         /**< Tape cartridge slots */
  kImportExport = 3,    /**< Mail slots / I/E bay */
  kDataTransfer = 4,    /**< Tape drives */
};

// ── CDB structures ───────────────────────────────────────────────────────────

/** MOVE MEDIUM command (opcode 0xA5, 12 bytes, SMC-3 §6.7) */
struct MoveMediumCDB {
  uint8_t opcode;            /**< 0xA5 */
  uint8_t reserved1;
  uint8_t transport_addr[2]; /**< Medium transport element addr (BE uint16) */
  uint8_t source_addr[2];    /**< Source element address (BE uint16) */
  uint8_t dest_addr[2];      /**< Destination element address (BE uint16) */
  uint8_t reserved2[2];
  uint8_t invert;   /**< bit0 = invert (usually 0) */
  uint8_t control;  /**< 0x00 */
};
static_assert(sizeof(MoveMediumCDB) == 12, "MoveMediumCDB must be 12 bytes");

/** READ ELEMENT STATUS command (opcode 0xB8, 12 bytes, SMC-3 §6.10) */
struct ReadElementStatusCDB {
  uint8_t opcode;              /**< 0xB8 */
  uint8_t element_type_voltag; /**< bits[3:0]=element type, bit4=VolTag */
  uint8_t start_element[2];    /**< First element address (BE uint16) */
  uint8_t num_elements[2];     /**< Number of elements to return (BE uint16) */
  uint8_t reserved;
  uint8_t alloc_length[3];     /**< Allocation length (BE uint24) */
  uint8_t reserved2;
  uint8_t control;
};
static_assert(sizeof(ReadElementStatusCDB) == 12,
              "ReadElementStatusCDB must be 12 bytes");

// ── Inventory data model ─────────────────────────────────────────────────────

/** Information about a single storage or I/E slot */
struct SlotInfo {
  uint16_t address;       /**< SCSI element address */
  uint16_t logical_slot;  /**< 1-based slot number */
  bool full;              /**< true when a cartridge is present */
  bool is_import_export;  /**< true for I/E (mail) slots */
  char barcode[37];       /**< Volume tag / barcode (NUL-terminated, max 36) */
};

/** Information about a single data-transfer element (tape drive) */
struct DriveInfo {
  uint16_t address;              /**< SCSI element address */
  uint16_t logical_drive;        /**< 0-based drive index */
  bool loaded;                   /**< true when a cartridge is in the drive */
  uint16_t loaded_from_address;  /**< Source element addr (valid if loaded) */
  uint16_t loaded_from_slot;     /**< 1-based logical slot (0 if unknown) */
  char barcode[37];              /**< Volume tag of loaded tape (may be empty) */
};

/** Complete picture of a changer's elements, returned by ReadElementStatus() */
struct ChangerInventory {
  uint16_t first_slot_address;   /**< SCSI address of logical slot 1 */
  uint16_t first_drive_address;  /**< SCSI address of logical drive 0 */
  uint16_t first_ie_address;     /**< SCSI address of first I/E slot */
  uint16_t num_slots;            /**< Total storage slots */
  uint16_t num_drives;           /**< Total data-transfer elements */
  uint16_t num_ie_slots;         /**< Total import/export slots */
  std::vector<SlotInfo> slots;   /**< Storage elements */
  std::vector<SlotInfo> ie_slots; /**< Import/export elements */
  std::vector<DriveInfo> drives; /**< Data-transfer elements */
};

// ── Public API ───────────────────────────────────────────────────────────────

/**
 * Read complete element status from an autochanger.
 *
 * @param fd            Open file descriptor for /dev/sgX
 * @param device_name   Device path (for error messages)
 * @param[out] inv      Populated inventory on success
 * @param request_volume_tags  Request barcodes (VolTag bit in CDB)
 * @return true on success, false on SCSI or parse error
 */
bool ReadElementStatus(int fd,
                       const char* device_name,
                       ChangerInventory& inv,
                       bool request_volume_tags = true);

/**
 * Move a medium between two SCSI element addresses.
 *
 * Use SCSI element addresses (from ChangerInventory), not logical numbers.
 * The transport element address is always 0 (first picker).
 *
 * @param fd            Open file descriptor for /dev/sgX
 * @param device_name   Device path (for error messages)
 * @param src_addr      Source SCSI element address
 * @param dst_addr      Destination SCSI element address
 * @return true on success, false on error
 *         Sets errno to ENOMEDIUM when source element is empty
 *         (SCSI sense: key=0x05, ASC=0x3B, ASCQ=0x0E)
 */
bool MoveMedium(int fd,
                const char* device_name,
                uint16_t src_addr,
                uint16_t dst_addr);

/**
 * Force changer to re-inventory all slots (INITIALIZE ELEMENT STATUS).
 *
 * @param fd            Open file descriptor for /dev/sgX
 * @param device_name   Device path (for error messages)
 * @return true on success, false on error
 */
bool InitializeElementStatus(int fd, const char* device_name);

// ── Helpers (also used by tests) ─────────────────────────────────────────────

/** Build a MoveMediumCDB from SCSI element addresses (all BE uint16). */
MoveMediumCDB BuildMoveMediumCDB(uint16_t transport_addr,
                                 uint16_t src_addr,
                                 uint16_t dst_addr);

/**
 * Parse a raw READ ELEMENT STATUS response buffer into a ChangerInventory.
 * Exposed for unit testing without real hardware.
 *
 * @param buf    Raw response bytes
 * @param len    Buffer length
 * @param[out] inv  Populated on success
 * @return true on success
 */
bool ParseElementStatusResponse(const uint8_t* buf,
                                std::size_t len,
                                ChangerInventory& inv);

} /* namespace bareos::scsi */

#endif  // HAVE_LOWLEVEL_SCSI_INTERFACE
#endif  // BAREOS_LIB_SCSI_CHANGER_H_
