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

#include "lib/device_inventory.h"

#include "include/bareos.h"
#include "lib/scan.h"
#include "lib/util.h"

namespace {
inline constexpr const char kFormat[]
    = "device name=%s changer=%s drive=%d autoselect=%d media_type=%s "
      "read_only=%d write_only=%d open=%d unmounted=%d blocked=%d "
      "reading=%d appending=%d writers=%d reserved=%d slot=%d volume=%s "
      "pool=%s\n";
inline constexpr const char kScan[]
    = "device name=%127s changer=%127s drive=%d autoselect=%d "
      "media_type=%127s read_only=%d write_only=%d open=%d unmounted=%d "
      "blocked=%d reading=%d appending=%d writers=%d reserved=%d slot=%d "
      "volume=%127s pool=%127s";
inline constexpr std::string_view kNone = "*None*";

std::string Encode(const std::string& value)
{
  if (value.empty()) { return std::string(kNone); }
  std::string encoded = value;
  BashSpaces(encoded);
  return encoded;
}

std::string Decode(const char* value)
{
  if (kNone == value) { return {}; }
  std::string decoded = value;
  for (auto& c : decoded) {
    if (c == 0x1) { c = ' '; }
  }
  return decoded;
}
}  // namespace

std::string FormatDeviceInventory(const DeviceInventoryEntry& e)
{
  PoolMem line;
  Mmsg(line, kFormat, Encode(e.device).c_str(), Encode(e.changer).c_str(),
       e.drive_index, e.autoselect, Encode(e.media_type).c_str(), e.read_only,
       e.write_only, e.open, e.unmounted, e.blocked, e.reading, e.appending,
       e.writers, e.reserved, e.slot, Encode(e.volume).c_str(),
       Encode(e.pool).c_str());
  return line.c_str();
}

std::optional<DeviceInventoryEntry> ParseDeviceInventory(const char* line)
{
  char device[128], changer[128], media_type[128], volume[128], pool[128];
  int autoselect, read_only, write_only, open, unmounted, blocked, reading,
      appending;
  DeviceInventoryEntry e;
  if (bsscanf(line, kScan, device, changer, &e.drive_index, &autoselect,
              media_type, &read_only, &write_only, &open, &unmounted, &blocked,
              &reading, &appending, &e.writers, &e.reserved, &e.slot, volume,
              pool)
      != 17) {
    return std::nullopt;
  }
  e.device = Decode(device);
  e.changer = Decode(changer);
  e.media_type = Decode(media_type);
  e.volume = Decode(volume);
  e.pool = Decode(pool);
  e.autoselect = autoselect != 0;
  e.read_only = read_only != 0;
  e.write_only = write_only != 0;
  e.open = open != 0;
  e.unmounted = unmounted != 0;
  e.blocked = blocked != 0;
  e.reading = reading != 0;
  e.appending = appending != 0;
  return e;
}
