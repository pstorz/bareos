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

/* Device inventory record sent by the storage daemon on the "inventory"
 * command. It reports facts about one device, the director decides. */

#ifndef BAREOS_LIB_DEVICE_INVENTORY_H_
#define BAREOS_LIB_DEVICE_INVENTORY_H_

#include <optional>
#include <string>
#include <string_view>

struct DeviceInventoryEntry {
  std::string device;
  std::string changer;  // empty: not part of an autochanger
  int drive_index{-1};  // only for autochanger drives
  bool autoselect{true};
  std::string media_type;
  bool read_only{false};
  bool write_only{false};
  bool open{false};
  bool unmounted{false};  // blocked by an operator unmount
  bool blocked{false};    // waiting for a mount or similar
  bool reading{false};
  bool appending{false};
  int writers{0};
  int reserved{0};
  int slot{0};         // 0: unknown or not loaded from a slot
  std::string volume;  // empty: no volume label read
  std::string pool;    // pool the device is reserved for

  bool operator==(const DeviceInventoryEntry&) const = default;
};

inline constexpr std::string_view kInventoryCommand = "inventory\n";

std::string FormatDeviceInventory(const DeviceInventoryEntry& entry);
std::optional<DeviceInventoryEntry> ParseDeviceInventory(const char* line);

#endif  // BAREOS_LIB_DEVICE_INVENTORY_H_
