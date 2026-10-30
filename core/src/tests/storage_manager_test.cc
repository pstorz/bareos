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

TEST(storage_manager, append_lease_is_exclusive)
{
  StorageModel model("Tape");
  SetupChanger(model);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kAlreadyHeld);
  EXPECT_EQ(model.AcquireLease(2, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kConflict);
  EXPECT_EQ(model.AcquireLease(2, "A1", AccessMode::kRead, "Drive-0"),
            LeaseResult::kConflict);
  EXPECT_TRUE(model.IsLeasedByOther("A1", 2));
  EXPECT_FALSE(model.IsLeasedByOther("A1", 1));

  model.ReleaseLeases(1);
  EXPECT_TRUE(model.LeasesOf("A1").empty());
  EXPECT_EQ(model.AcquireLease(2, "A1", AccessMode::kAppend, "Drive-0"),
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

TEST(storage_manager, drive_cannot_serve_two_volumes)
{
  StorageModel model("Tape");
  SetupChanger(model);
  EXPECT_EQ(model.AcquireLease(1, "A1", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kGranted);
  EXPECT_EQ(model.AcquireLease(2, "A2", AccessMode::kAppend, "Drive-0"),
            LeaseResult::kDriveBusy);
  EXPECT_EQ(model.AcquireLease(2, "A2", AccessMode::kAppend, "Unknown"),
            LeaseResult::kUnknownDrive);
}

TEST(storage_manager, concurrent_append_leases_grant_exactly_one)
{
  StorageModel model("Tape");
  SetupChanger(model);
  std::atomic<int> granted{0};
  std::vector<std::thread> threads;
  for (JobId job = 1; job <= 16; ++job) {
    threads.emplace_back([&, job] {
      if (model.AcquireLease(job, "A1", AccessMode::kAppend, "Drive-0")
          == LeaseResult::kGranted) {
        ++granted;
      }
    });
  }
  for (auto& t : threads) { t.join(); }
  EXPECT_EQ(granted, 1);
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
      "  Drive-0: volume \"A1\" from slot 1, JobId 42 (append)",
      "  Drive-1: empty, command pending",
      "  Slot 1: empty",
      "  Slot 2: \"A2\"",
      "  Slot 3: empty",
  };
  EXPECT_EQ(DescribeModel(model), expected);
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
