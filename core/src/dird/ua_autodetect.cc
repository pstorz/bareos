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
 * @file
 * Director "autodetect changers" console command.
 *
 * Queries one or all configured Storage Daemons for physically attached
 * tape libraries using the "detect changers" SD command, then displays
 * an inventory and optionally generates ready-to-paste config snippets.
 *
 * Usage:
 *   autodetect changers [storage=<name>] [generate=yes]
 */

#include "include/bareos.h"
#include "dird/ua_autodetect.h"
#include "dird/dird_conf.h"
#include "dird/dird_globals.h"
#include "dird/sd_cmds.h"
#include "dird/storage.h"
#include "dird/ua.h"
#include "lib/bsock.h"
#include "lib/parse_conf.h"
#include "dird/ua_select.h"

#include <string>
#include <string_view>
#include <vector>

namespace directordaemon {

// ---------------------------------------------------------------------------
// Data structures for parsed SD response
// ---------------------------------------------------------------------------

struct DetectedDrive {
  int index{0};
  std::string sg_path;
  std::string st_path;
};

struct DetectedSlot {
  int num{0};
  bool full{false};
  std::string barcode;
};

struct DetectedChanger {
  std::string sg_path;
  std::string vendor;
  std::string product;
  std::string serial;
  int slots{0};
  int drives{0};
  std::vector<DetectedDrive> drive_list;
  std::vector<DetectedSlot> slot_list;
};

// ---------------------------------------------------------------------------
// Key=value parsing helpers
// ---------------------------------------------------------------------------

/** Extract the value for @p key from a space-separated key=value line. */
static std::string ExtractValue(std::string_view line, std::string_view key)
{
  std::string needle = std::string(key) + "=";
  auto pos = line.find(needle);
  if (pos == std::string_view::npos) { return {}; }
  pos += needle.size();
  auto end = line.find(' ', pos);
  return std::string(line.substr(pos, end - pos));
}

/** Parse a 3000 changer line into @p changer. */
static void ParseChangerLine(std::string_view line, DetectedChanger& changer)
{
  changer.sg_path = ExtractValue(line, "sg");
  changer.vendor = ExtractValue(line, "vendor");
  changer.product = ExtractValue(line, "product");
  changer.serial = ExtractValue(line, "serial");
  auto slots_str = ExtractValue(line, "slots");
  auto drives_str = ExtractValue(line, "drives");
  if (!slots_str.empty()) { changer.slots = std::stoi(slots_str); }
  if (!drives_str.empty()) { changer.drives = std::stoi(drives_str); }
}

/** Parse a 3001 drive line into @p drv. */
static void ParseDriveLine(std::string_view line, DetectedDrive& drv)
{
  auto idx_str = ExtractValue(line, "index");
  if (!idx_str.empty()) { drv.index = std::stoi(idx_str); }
  drv.sg_path = ExtractValue(line, "sg");
  drv.st_path = ExtractValue(line, "st");
}

/** Parse a 3002 slot line into @p slot. */
static void ParseSlotLine(std::string_view line, DetectedSlot& slot)
{
  auto num_str = ExtractValue(line, "num");
  if (!num_str.empty()) { slot.num = std::stoi(num_str); }
  auto full_str = ExtractValue(line, "full");
  slot.full = (full_str == "1");
  slot.barcode = ExtractValue(line, "barcode");
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

/** Print human-readable inventory for all detected changers on @p store. */
static void DisplayChangers(UaContext* ua,
                            const std::vector<DetectedChanger>& changers,
                            StorageResource* store)
{
  if (changers.empty()) {
    ua->SendMsg("No tape changers detected on Storage \"%s\".\n",
                store->resource_name_);
    return;
  }

  ua->SendMsg("Detected changers on Storage \"%s\":\n",
              store->resource_name_);

  for (const auto& changer : changers) {
    ua->SendMsg("  Changer %s (%s %s, Serial: %s)\n",
                changer.sg_path.c_str(), changer.vendor.c_str(),
                changer.product.c_str(), changer.serial.c_str());
    ua->SendMsg("    Slots:  %d\n", changer.slots);
    ua->SendMsg("    Drives: %d\n", changer.drives);

    for (const auto& drv : changer.drive_list) {
      ua->SendMsg("    Drive %d: sg=%-12s  st=%s\n", drv.index,
                  drv.sg_path.c_str(), drv.st_path.c_str());
    }

    for (const auto& slot : changer.slot_list) {
      if (slot.full) {
        ua->SendMsg("    Slot %3d: [FULL ]  %s\n", slot.num,
                    slot.barcode.c_str());
      } else {
        ua->SendMsg("    Slot %3d: [empty]\n", slot.num);
      }
    }
  }
}

/** Print bareos-sd.conf / bareos-dir.conf snippets for all changers. */
static void GenerateConfigSnippets(UaContext* ua,
                                   const std::vector<DetectedChanger>& changers)
{
  int changer_index = 0;
  for (const auto& changer : changers) {
    std::string autochanger_name
        = "Autochanger-" + std::to_string(changer_index);

    ua->SendMsg("\n# --- Generated config snippet for %s ---\n",
                changer.sg_path.c_str());
    ua->SendMsg("# Add to /etc/bareos/bareos-sd.d/autochanger/ :\n\n");

    // Build comma-separated drive name list
    std::string drive_names;
    for (int i = 0; i < static_cast<int>(changer.drive_list.size()); ++i) {
      if (i > 0) { drive_names += ", "; }
      drive_names += "Drive-" + std::to_string(i);
    }

    ua->SendMsg("Autochanger {\n");
    ua->SendMsg("  Name = \"%s\"\n", autochanger_name.c_str());
    ua->SendMsg("  UseNativeScsi = yes\n");
    ua->SendMsg("  ChangerDevice = %s\n", changer.sg_path.c_str());
    ua->SendMsg("  Device = %s\n", drive_names.c_str());
    ua->SendMsg("}\n\n");

    for (int i = 0; i < static_cast<int>(changer.drive_list.size()); ++i) {
      const auto& drv = changer.drive_list[i];
      ua->SendMsg("Device {\n");
      ua->SendMsg("  Name = Drive-%d\n", i);
      ua->SendMsg("  DeviceType = tape\n");
      ua->SendMsg("  ArchiveDevice = %s\n", drv.st_path.c_str());
      ua->SendMsg("  MediaType = LTO\n");
      ua->SendMsg("  AutoChanger = yes\n");
      ua->SendMsg("}\n\n");
    }

    ua->SendMsg("# --- Add to /etc/bareos/bareos-dir.d/storage/ ---\n\n");
    ua->SendMsg("Storage {\n");
    ua->SendMsg("  Name = \"%s\"\n", autochanger_name.c_str());
    ua->SendMsg("  Address = <bareos-sd-hostname>\n");
    ua->SendMsg("  Password = \"<bareos-sd-password>\"\n");
    ua->SendMsg("  Device = %s\n", autochanger_name.c_str());
    ua->SendMsg("  MediaType = LTO\n");
    ua->SendMsg("  AutoChanger = yes\n");
    ua->SendMsg("}\n");

    ++changer_index;
  }
}

// ---------------------------------------------------------------------------
// Per-storage query
// ---------------------------------------------------------------------------

/** Query @p store for attached changers and display the result. */
static void QueryStorage(UaContext* ua,
                         StorageResource* store,
                         bool generate)
{
  UnifiedStorageResource lstore;
  lstore.store = store;
  PmStrcpy(lstore.store_source, T_("command line"));
  SetWstorage(ua->jcr, &lstore);

  BareosSocket* sd = open_sd_bsock(ua);
  if (!sd) {
    ua->ErrorMsg("Failed to connect to Storage \"%s\".\n",
                 store->resource_name_);
    return;
  }

  sd->fsend("detect changers\n");

  std::vector<DetectedChanger> changers;
  DetectedChanger* current = nullptr;

  while (sd->recv() >= 0) {
    const char* line = sd->msg;
    if (strncmp(line, "3000 ", 5) == 0) {
      changers.push_back(DetectedChanger{});
      current = &changers.back();
      ParseChangerLine(line + 5, *current);
    } else if (strncmp(line, "3001 ", 5) == 0 && current) {
      DetectedDrive drv{};
      ParseDriveLine(line + 5, drv);
      current->drive_list.push_back(drv);
    } else if (strncmp(line, "3002 ", 5) == 0 && current) {
      DetectedSlot slot{};
      ParseSlotLine(line + 5, slot);
      current->slot_list.push_back(slot);
    }
    // 3099 OK — end of response; loop exits on next recv() < 0
  }

  CloseSdBsock(ua);

  DisplayChangers(ua, changers, store);

  if (generate && !changers.empty()) { GenerateConfigSnippets(ua, changers); }
}

// ---------------------------------------------------------------------------
// Public command entry point
// ---------------------------------------------------------------------------

bool AutodetectChangersCmd(UaContext* ua, const char* /* cmd */)
{
  // Parse optional storage= argument
  int storage_arg = FindArgWithValue(ua, NT_("storage"));
  const char* storage_name = (storage_arg >= 0) ? ua->argv[storage_arg] : nullptr;

  // Parse optional generate=yes argument
  int gen_arg = FindArgWithValue(ua, NT_("generate"));
  bool generate = (gen_arg >= 0 && Bstrcasecmp(ua->argv[gen_arg], "yes"));

  if (storage_name) {
    // Query a single named storage resource
    StorageResource* store = ua->GetStoreResWithName(storage_name);
    if (!store) {
      ua->ErrorMsg("Storage resource \"%s\" not found.\n", storage_name);
      return false;
    }
    QueryStorage(ua, store, generate);
  } else {
    // Query all configured storage resources
    StorageResource* store = nullptr;
    foreach_res (store, R_STORAGE) {
      if (!ua->AclAccessOk(Storage_ACL, store->resource_name_)) { continue; }
      QueryStorage(ua, store, generate);
    }
  }

  return true;
}

}  // namespace directordaemon
