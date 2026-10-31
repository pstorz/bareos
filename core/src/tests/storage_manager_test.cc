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

#include "gtest/gtest.h"

#include <thread>

#include "dird/storage_manager.h"
#include "dird/volume_status_policy.h"
#include "include/dir_sd_protocol.h"

using namespace directordaemon::storage_manager;

namespace {
DriveState Drive(std::string name)
{
  DriveState drive;
  drive.name = std::move(name);
  return drive;
}

void SetupChanger(StorageModel& model)
{
  model.SetInventory({Drive("Drive-0"), Drive("Drive-1")},
                     {{1, "A1"}, {2, "A2"}, {3, std::nullopt}});
}
}  // namespace

TEST(storage_manager, prefers_drive_holding_volume)
{
  StorageModel model("Tape");
  SetupChanger(model);
  ASSERT_TRUE(model.VolumeLoaded("Drive-1", "A2", 2));

  auto choice = model.ChooseDrive("A2");
  ASSERT_TRUE(choice);
  EXPECT_EQ(choice->drive, "Drive-1");
  EXPECT_EQ(choice->kind, DriveCandidate::kVolumeLoaded);

  choice = model.ChooseDrive("A1");
  ASSERT_TRUE(choice);
  EXPECT_EQ(choice->drive, "Drive-0");
  EXPECT_EQ(choice->kind, DriveCandidate::kEmpty);
}

TEST(storage_manager, no_drive_while_volume_drive_is_pending)
{
  StorageModel model("Tape");
  SetupChanger(model);
  model.VolumeLoaded("Drive-0", "A1", 1);
  model.MarkPending("Drive-0", true);
  EXPECT_FALSE(model.ChooseDrive("A1"));
}

TEST(storage_manager, falls_back_to_drive_needing_unload)
{
  StorageModel model("Tape");
  SetupChanger(model);
  model.VolumeLoaded("Drive-0", "A1", 1);
  model.VolumeLoaded("Drive-1", "A2", 2);
  ASSERT_EQ(model.AcquireLease(7, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);

  auto choice = model.ChooseDrive("B9");
  ASSERT_TRUE(choice);
  EXPECT_EQ(choice->drive, "Drive-1");
  EXPECT_EQ(choice->kind, DriveCandidate::kNeedsUnload);
}

TEST(storage_manager, appending_jobs_share_volume_on_one_drive)
{
  StorageModel model("Tape");
  SetupChanger(model);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kAlreadyHeld);
  EXPECT_EQ(model.AcquireLease(2, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(3, "A1", AccessMode::kAppend, "Drive-1"),
            LeaseResult::kConflict);
  EXPECT_EQ(model.AcquireLease(3, "A1", AccessMode::kRead, "Drive-0"),
            LeaseResult::kConflict);
  EXPECT_TRUE(model.IsLeasedByOther("A1", 3));
  EXPECT_EQ(model.LeasesOf("A1").size(), 2u);

  model.ReleaseLeases(1);
  model.ReleaseLeases(2);
  EXPECT_TRUE(model.LeasesOf("A1").empty());
  EXPECT_EQ(model.AcquireLease(3, "A1", AccessMode::kRead, "Drive-1"),
            LeaseResult::kGranted);
}

TEST(storage_manager, read_leases_share_volume_on_same_drive_only)
{
  StorageModel model("Tape");
  SetupChanger(model);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kRead, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(2, "A1", AccessMode::kRead, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(3, "A1", AccessMode::kRead, "Drive-1"),
            LeaseResult::kConflict);
  EXPECT_EQ(model.LeasesOf("A1").size(), 2u);
}

TEST(storage_manager, new_volume_replaces_lease_of_same_mode)
{
  StorageModel model("Tape");
  SetupChanger(model);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(1, "B1", AccessMode::kRead, "Drive-1"),
            LeaseResult::kGranted);
  // volume change during the job, another job still uses the drive
  EXPECT_EQ(model.AcquireLease(2, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(1, "A2", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.LeasesOf("A1").size(), 1u);
  EXPECT_EQ(model.LeasesOf("B1").size(), 1u);

  // appending on the other drive removes the job from its former drive
  EXPECT_EQ(model.AcquireLease(1, "A3", AccessMode::kAppend, "Drive-1"),
            LeaseResult::kGranted);
  EXPECT_TRUE(model.LeasesOf("A2").empty());
  EXPECT_EQ(model.Drives()[0].jobs, std::vector<JobId>{2});
  EXPECT_EQ(model.Drives()[1].jobs, std::vector<JobId>{1});
  EXPECT_EQ(model.AcquireLease(2, "A2", AccessMode::kAppend, "Unknown"),
            LeaseResult::kUnknownDrive);
}

TEST(storage_manager, concurrent_leases_bind_volume_to_one_drive)
{
  StorageModel model("Tape");
  SetupChanger(model);
  std::atomic<int> drive0{0};
  std::atomic<int> drive1{0};
  std::vector<std::thread> threads;
  for (JobId job = 1; job <= 16; ++job) {
    threads.emplace_back([&, job] {
      std::string drive = job % 2 ? "Drive-0" : "Drive-1";
      if (model.AcquireLease(job, "A1", AccessMode::kAppend, drive)
          == LeaseResult::kGranted) {
        ++(job % 2 ? drive0 : drive1);
      }
    });
  }
  for (auto& t : threads) { t.join(); }
  EXPECT_TRUE((drive0 == 8 && drive1 == 0) || (drive0 == 0 && drive1 == 8));
}

TEST(storage_manager, load_and_unload_track_slots)
{
  StorageModel model("Tape");
  SetupChanger(model);
  model.MarkPending("Drive-0", true);
  ASSERT_TRUE(model.VolumeLoaded("Drive-0", "A1", 1));
  EXPECT_FALSE(model.Drives()[0].pending);
  EXPECT_FALSE(model.Slots()[0].volume);
  EXPECT_EQ(model.SlotOfVolume("A1"), 1);

  ASSERT_TRUE(model.VolumeUnloaded("Drive-0"));
  EXPECT_EQ(model.Slots()[0].volume, "A1");
  EXPECT_FALSE(model.Drives()[0].loaded_volume);
  EXPECT_FALSE(model.VolumeUnloaded("Unknown"));
}

TEST(storage_manager, inventory_keeps_jobs_and_drops_stale_leases)
{
  StorageModel model("Tape");
  SetupChanger(model);
  model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0");
  model.AcquireLease(2, "A2", AccessMode::kAppend, "Drive-1");

  model.SetInventory({Drive("Drive-0")}, {});
  ASSERT_EQ(model.Drives().size(), 1u);
  EXPECT_EQ(model.Drives()[0].jobs, std::vector<JobId>{1});
  EXPECT_EQ(model.LeasesOf("A1").size(), 1u);
  EXPECT_TRUE(model.LeasesOf("A2").empty());
}

TEST(storage_manager, finds_catalog_divergences)
{
  StorageModel model("Tape");
  SetupChanger(model);
  model.VolumeLoaded("Drive-0", "A1", 1);

  std::vector<CatalogVolumeLocation> catalog{
      {"A1", 1, true},   // in drive, home slot 1: consistent
      {"A2", 5, true},   // wrong slot
      {"C1", 3, true},   // catalog thinks it is in the changer
      {"C2", 0, false},  // consistently not in the changer
  };
  auto divergences = model.FindDivergences(catalog);
  ASSERT_EQ(divergences.size(), 2u);
  EXPECT_EQ(divergences[0].volume, "A2");
  EXPECT_EQ(divergences[0].catalog_slot, 5);
  EXPECT_EQ(divergences[0].actual_slot, 2);
  EXPECT_EQ(divergences[1].volume, "C1");
  EXPECT_FALSE(divergences[1].in_storage);

  model.SetInventory({}, {{4, "NEW"}});
  divergences = model.FindDivergences({});
  ASSERT_EQ(divergences.size(), 1u);
  EXPECT_EQ(divergences[0].volume, "NEW");
  EXPECT_FALSE(divergences[0].catalog_slot);
  EXPECT_TRUE(divergences[0].in_storage);
}

TEST(storage_manager, registry_returns_same_model)
{
  StorageManager manager;
  auto a = manager.Get("Tape");
  EXPECT_EQ(a, manager.Get("Tape"));
  EXPECT_EQ(a, manager.Find("Tape"));
  EXPECT_FALSE(manager.Find("File"));
  EXPECT_EQ(manager.StorageNames(), std::vector<std::string>{"Tape"});
  manager.Remove("Tape");
  EXPECT_FALSE(manager.Find("Tape"));
}

TEST(storage_manager, builds_inventory_from_changer_elements)
{
  using Type = ChangerElement::Type;
  std::vector<ChangerElement> elements{
      {Type::kDrive, 0, 3, "A3"},
      {Type::kDrive, 1, std::nullopt, std::nullopt},
      {Type::kSlot, 2, std::nullopt, "A2"},
      {Type::kSlot, 1, std::nullopt, std::nullopt},
      {Type::kImportExport, 10, std::nullopt, "IO1"},
  };
  auto inventory = BuildInventory(elements);

  ASSERT_EQ(inventory.drives.size(), 2u);
  EXPECT_EQ(inventory.drives[0].name, "Drive-0");
  EXPECT_EQ(inventory.drives[0].loaded_volume, "A3");
  EXPECT_EQ(inventory.drives[0].loaded_from_slot, 3);
  EXPECT_EQ(inventory.drives[1].name, DriveName(1));
  EXPECT_FALSE(inventory.drives[1].loaded_volume);

  ASSERT_EQ(inventory.slots.size(), 3u);
  EXPECT_EQ(inventory.slots[0].slot, 1);
  EXPECT_FALSE(inventory.slots[0].volume);
  EXPECT_EQ(inventory.slots[1].volume, "A2");
  EXPECT_EQ(inventory.slots[2].slot, 10);
  EXPECT_EQ(inventory.slots[2].volume, "IO1");
}

TEST(storage_manager, describes_model)
{
  StorageModel model("Tape");
  SetupChanger(model);
  model.VolumeLoaded("Drive-0", "A1", 1);
  model.AcquireLease(42, "A1", AccessMode::kAppend, "Drive-0");
  model.MarkPending("Drive-1", true);

  std::vector<std::string> expected{
      "Storage \"Tape\": 2 drive(s)",
      "  Drive-0: volume \"A1\" from slot 1, JobId 42 (append \"A1\")",
      "  Drive-1: empty, command pending",
      "  Slot 1: empty",
      "  Slot 2: \"A2\"",
      "  Slot 3: empty",
  };
  EXPECT_EQ(DescribeModel(model), expected);
}

TEST(storage_manager, binds_devices_to_drives)
{
  StorageModel model("Tape");
  SetupChanger(model);
  EXPECT_EQ(model.BindDevice("tape0", 0), "Drive-0");
  EXPECT_EQ(model.BindDevice("tape1", 1), "Drive-1");
  EXPECT_EQ(model.BindDevice("tape0", 0), "Drive-0");
  // a second device claiming an already bound drive index is kept apart
  EXPECT_EQ(model.BindDevice("other", 0), "other");
  EXPECT_EQ(model.BindDevice("other", std::nullopt), "other");
  EXPECT_EQ(model.BindDevice("file", std::nullopt), "file");

  // inventory refresh keeps device bindings, standalone devices and leases
  ASSERT_EQ(model.AcquireLease(1, "F1", AccessMode::kAppend, "file"),
            LeaseResult::kGranted);
  SetupChanger(model);
  auto drives = model.Drives();
  ASSERT_EQ(drives.size(), 4u);
  EXPECT_EQ(drives[0].device, "tape0");
  EXPECT_FALSE(drives[2].changer);
  EXPECT_EQ(model.LeasesOf("F1").size(), 1u);
  // standalone devices are never chosen for changer volumes
  model.VolumeLoaded("Drive-0", "A1", 1);
  model.VolumeLoaded("Drive-1", "A2", 2);
  model.AcquireLease(2, "A1", AccessMode::kAppend, "Drive-0");
  model.AcquireLease(3, "A2", AccessMode::kAppend, "Drive-1");
  EXPECT_FALSE(model.ChooseDrive("B9"));

  EXPECT_EQ(DescribeModel(model)[1],
            "  Drive-0: volume \"A1\" from slot 1, device \"tape0\", JobId 2 "
            "(append \"A1\")");
}

TEST(storage_manager, registry_releases_leases_of_job)
{
  StorageManager manager;
  auto tape = manager.Get("Tape");
  auto file = manager.Get("File");
  SetupChanger(*tape);
  std::string drive = file->BindDevice("FileStorage", std::nullopt);
  tape->AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0");
  file->AcquireLease(1, "F1", AccessMode::kAppend, drive);
  file->AcquireLease(2, "F2", AccessMode::kAppend, drive);

  manager.ReleaseLeases(1);
  EXPECT_TRUE(tape->Leases().empty());
  EXPECT_TRUE(file->LeasesOf("F1").empty());
  EXPECT_EQ(file->LeasesOf("F2").size(), 1u);
}

TEST(storage_manager, describes_divergences)
{
  EXPECT_EQ(DescribeDivergence({"A1", 5, true, 2}),
            "Volume \"A1\": catalog slot 5, storage daemon slot 2");
  EXPECT_EQ(DescribeDivergence({"A1", 5, false, std::nullopt}),
            "Volume \"A1\": catalog slot 5, storage daemon not present");
  EXPECT_EQ(DescribeDivergence({"A1", std::nullopt, true, std::nullopt}),
            "Volume \"A1\": catalog not present, storage daemon unknown slot");
}

namespace {
using directordaemon::ResolveVolumeStatus;

// stale "Append" from the storage daemon must not undo director decisions
static_assert(ResolveVolumeStatus("Used", "Append", false) == "Used");
static_assert(ResolveVolumeStatus("Read-Only", "Append", false) == "Read-Only");
static_assert(ResolveVolumeStatus("Disabled", "Append", false) == "Disabled");
static_assert(ResolveVolumeStatus("Purged", "Append", false) == "Purged");
static_assert(ResolveVolumeStatus("Full", "Append", false) == "Full");
static_assert(ResolveVolumeStatus("Error", "Full", false) == "Error");
// storage daemon observations are accepted
static_assert(ResolveVolumeStatus("Append", "Full", false) == "Full");
static_assert(ResolveVolumeStatus("Append", "Error", false) == "Error");
static_assert(ResolveVolumeStatus("Used", "Error", false) == "Error");
static_assert(ResolveVolumeStatus("Purged", "Append", true) == "Append");
static_assert(ResolveVolumeStatus("Recycle", "Append", true) == "Append");
static_assert(ResolveVolumeStatus("Unlabeled", "Append", false) == "Append");
// relabel only grants Append, nothing else
static_assert(ResolveVolumeStatus("Used", "Recycle", true) == "Used");
static_assert(ResolveVolumeStatus("Append", "Append", false) == "Append");
}  // namespace

namespace {
DeviceInventoryEntry ChangerDevice(std::string name, int index)
{
  DeviceInventoryEntry entry;
  entry.device = std::move(name);
  entry.changer = "Tape Changer";
  entry.drive_index = index;
  entry.media_type = "LTO";
  return entry;
}
}  // namespace

TEST(device_inventory, format_and_parse_round_trip)
{
  DeviceInventoryEntry entry = ChangerDevice("Tape Drive 1", 1);
  entry.autoselect = false;
  entry.open = true;
  entry.appending = true;
  entry.writers = 2;
  entry.reserved = 1;
  entry.slot = 7;
  entry.volume = "A 7";
  entry.pool = "Full Pool";

  auto parsed = ParseDeviceInventory(FormatDeviceInventory(entry).c_str());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, entry);

  DeviceInventoryEntry empty;
  empty.device = "File";
  parsed = ParseDeviceInventory(FormatDeviceInventory(empty).c_str());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, empty);

  EXPECT_FALSE(ParseDeviceInventory("3900 Invalid command: inventory\n"));
}

TEST(device_inventory, selects_devices_of_storage)
{
  DeviceInventoryEntry single;
  single.device = "File";
  std::vector<DeviceInventoryEntry> inventory{ChangerDevice("D0", 0),
                                              ChangerDevice("D1", 1), single};

  auto selected = DevicesOfStorage(inventory, {"Tape Changer"});
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_EQ(selected[1].device, "D1");

  selected = DevicesOfStorage(inventory, {"File", "D0"});
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_EQ(selected[0].device, "D0");
  EXPECT_EQ(selected[1].device, "File");
}

TEST(storage_manager, device_inventory_updates_drives)
{
  StorageModel model("Tape");
  SetupChanger(model);
  ASSERT_TRUE(model.VolumeLoaded("Drive-0", "A1", 1));

  DeviceInventoryEntry d0 = ChangerDevice("D0", 0);  // label not read yet
  DeviceInventoryEntry d1 = ChangerDevice("D1", 1);
  d1.volume = "A2";
  d1.slot = 2;
  d1.unmounted = true;
  DeviceInventoryEntry single;
  single.device = "File";
  single.volume = "F1";
  model.UpdateDevices({d0, d1, single});

  auto drives = model.Drives();
  ASSERT_EQ(drives.size(), 3u);
  EXPECT_EQ(drives[0].device, "D0");
  EXPECT_EQ(drives[0].loaded_volume, "A1");
  EXPECT_EQ(drives[1].device, "D1");
  EXPECT_EQ(drives[1].loaded_volume, "A2");
  EXPECT_EQ(drives[1].loaded_from_slot, 2);
  EXPECT_FALSE(drives[1].enabled);
  EXPECT_EQ(drives[2].name, "File");
  EXPECT_FALSE(drives[2].changer);
  EXPECT_EQ(drives[2].loaded_volume, "F1");
  EXPECT_EQ(model.BindDevice("D1", 1), "Drive-1");
  EXPECT_EQ(model.LoadedVolume("Drive-1"), "A2");
  EXPECT_FALSE(model.LoadedVolume("Unknown"));

  single.volume.clear();
  model.UpdateDevices({single});
  EXPECT_FALSE(model.Drives()[2].loaded_volume);

  // a changer inventory keeps the device reports
  SetupChanger(model);
  ASSERT_TRUE(model.Drives()[1].sd);
  EXPECT_TRUE(model.Drives()[1].sd->unmounted);
}

TEST(storage_manager, orders_devices_for_job)
{
  DeviceInventoryEntry d0 = ChangerDevice("D0", 0);
  d0.writers = 1;
  d0.pool = "Full";
  d0.volume = "A1";
  DeviceInventoryEntry d1 = ChangerDevice("D1", 1);
  d1.volume = "A2";
  DeviceInventoryEntry d2 = ChangerDevice("D2", 2);
  DeviceInventoryEntry d3 = ChangerDevice("D3", 3);
  d3.autoselect = false;
  DeviceInventoryEntry d4 = ChangerDevice("D4", 4);
  d4.read_only = true;
  DeviceInventoryEntry other;
  other.device = "Other";
  other.media_type = "File";
  std::vector<DeviceInventoryEntry> inventory{d0, d1, d2, d3, d4, other};

  DeviceRequest request{.append = true, .pool = "Full", .media_type = "LTO"};
  using Order = std::vector<std::string>;
  EXPECT_EQ(OrderDevices(inventory, {"Tape Changer"}, request),
            (Order{"D2", "D1", "D0"}));

  request.prefer_mounted = true;
  EXPECT_EQ(OrderDevices(inventory, {"Tape Changer"}, request),
            (Order{"D0", "D2", "D1"}));

  request.append = false;
  EXPECT_EQ(OrderDevices(inventory, {"Tape Changer"}, request),
            (Order{"D1", "D2", "D4", "D0"}));

  // explicitly named devices are used even without autoselect
  EXPECT_EQ(OrderDevices(inventory, {"D3", "Other"}, request), (Order{"D3"}));
  EXPECT_TRUE(OrderDevices(inventory, {"Unknown"}, request).empty());
}

static_assert(CheckJobCommand("JobId=1 BackupFormat=native\n", -1, 20, 0)
              == JobCommandCheck::kVersionMismatch);
static_assert(CheckJobCommand("JobId=1 SdProtocolVersion=2\n",
                              20,
                              20,
                              kDirSdProtocolVersion)
              == JobCommandCheck::kOk);
static_assert(CheckJobCommand("JobId=1 SdProtocolVersion=99\n", 20, 20, 99)
              == JobCommandCheck::kVersionMismatch);
static_assert(CheckJobCommand("JobId= SdProtocolVersion=1\n", -1, 20, 1)
              == JobCommandCheck::kMalformed);

// free drives first, so jobs spread over the drives
static_assert(DevicePreference({}) < DevicePreference({.has_volume = true}));
static_assert(DevicePreference({.has_volume = true, .same_pool = true})
              < DevicePreference({.has_volume = true}));
static_assert(DevicePreference({.has_volume = true})
              < DevicePreference({.same_pool = true, .users = 1}));
static_assert(DevicePreference({.same_pool = true, .users = 1})
              < DevicePreference({.users = 1}));
// with prefer mounted volumes, share a drive appending to the same pool
static_assert(
    DevicePreference({.prefer_mounted = true, .same_pool = true, .users = 1})
    < DevicePreference(
        {.prefer_mounted = true, .has_volume = true, .same_pool = true}));
static_assert(DevicePreference({.prefer_mounted = true,
                                .has_volume = true,
                                .same_pool = true})
              < DevicePreference({.prefer_mounted = true}));
static_assert(DevicePreference({.prefer_mounted = true})
              < DevicePreference({.prefer_mounted = true, .users = 1}));
// reading and unmounted drives last
static_assert(DevicePreference({.users = 1})
              < DevicePreference({.reading = true}));
static_assert(DevicePreference({.reading = true})
              < DevicePreference({.unavailable = true}));
static_assert(DevicePreference({.append_request = false})
              < DevicePreference({.append_request = false, .users = 1}));

static_assert(directordaemon::VolumeListContains("A,B,C", "B"));
static_assert(directordaemon::VolumeListContains("A", "A"));
static_assert(!directordaemon::VolumeListContains("AB,C", "A"));
static_assert(!directordaemon::VolumeListContains("", "A"));

using directordaemon::CanAppendToLoadedVolume;
static_assert(CanAppendToLoadedVolume(
    {.status = "Append", .same_pool = true, .same_media_type = true}));
static_assert(!CanAppendToLoadedVolume(
    {.status = "Full", .same_pool = true, .same_media_type = true}));
static_assert(!CanAppendToLoadedVolume({.status = "Append",
                                        .enabled = false,
                                        .same_pool = true,
                                        .same_media_type = true}));
static_assert(!CanAppendToLoadedVolume({.status = "Append",
                                        .same_media_type = true}));
static_assert(!CanAppendToLoadedVolume({.status = "Append",
                                        .same_pool = true}));
static_assert(!CanAppendToLoadedVolume({.status = "Append",
                                        .same_pool = true,
                                        .same_media_type = true,
                                        .unwanted = true}));
