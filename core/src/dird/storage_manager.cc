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

#include "dird/storage_manager.h"

#include <algorithm>
#include <set>

namespace directordaemon::storage_manager {

static_assert(!LeasesConflict(AccessMode::kRead, AccessMode::kRead));
static_assert(LeasesConflict(AccessMode::kRead, AccessMode::kAppend));
static_assert(LeasesConflict(AccessMode::kAppend, AccessMode::kRead));
static_assert(LeasesConflict(AccessMode::kAppend, AccessMode::kAppend));

static_assert(ClassifyDrive({}) == DriveCandidate::kEmpty);
static_assert(ClassifyDrive({.enabled = false}) == DriveCandidate::kUnusable);
static_assert(ClassifyDrive({.pending = true, .holds_requested_volume = true})
              == DriveCandidate::kUnusable);
static_assert(ClassifyDrive({.in_use = true,
                             .has_volume = true,
                             .holds_requested_volume = true})
              == DriveCandidate::kVolumeLoaded);
static_assert(ClassifyDrive({.in_use = true, .has_volume = true})
              == DriveCandidate::kUnusable);
static_assert(ClassifyDrive({.has_volume = true})
              == DriveCandidate::kNeedsUnload);
static_assert(Rank(DriveCandidate::kVolumeLoaded)
              > Rank(DriveCandidate::kEmpty));
static_assert(Rank(DriveCandidate::kEmpty)
              > Rank(DriveCandidate::kNeedsUnload));
static_assert(Rank(DriveCandidate::kNeedsUnload)
              > Rank(DriveCandidate::kUnusable));

DriveState* StorageModel::FindDrive(const std::string& name)
{
  auto it = std::find_if(drives_.begin(), drives_.end(),
                         [&](const DriveState& d) { return d.name == name; });
  return it == drives_.end() ? nullptr : &*it;
}

const DriveState* StorageModel::FindDrive(const std::string& name) const
{
  auto it = std::find_if(drives_.begin(), drives_.end(),
                         [&](const DriveState& d) { return d.name == name; });
  return it == drives_.end() ? nullptr : &*it;
}

void StorageModel::SetInventory(std::vector<DriveState> drives,
                                std::vector<SlotState> slots)
{
  std::lock_guard lock(mutex_);
  for (auto& drive : drives) {
    if (const DriveState* old = FindDrive(drive.name)) {
      drive.jobs = old->jobs;
    }
  }
  drives_ = std::move(drives);
  slots_ = std::move(slots);

  for (auto it = leases_.begin(); it != leases_.end();) {
    if (FindDrive(it->second.drive)) {
      ++it;
    } else {
      it = leases_.erase(it);
    }
  }
}

std::vector<DriveState> StorageModel::Drives() const
{
  std::lock_guard lock(mutex_);
  return drives_;
}

std::vector<SlotState> StorageModel::Slots() const
{
  std::lock_guard lock(mutex_);
  return slots_;
}

std::optional<int> StorageModel::SlotOfVolume(const std::string& volume) const
{
  std::lock_guard lock(mutex_);
  for (const auto& slot : slots_) {
    if (slot.volume == volume) { return slot.slot; }
  }
  for (const auto& drive : drives_) {
    if (drive.loaded_volume == volume) { return drive.loaded_from_slot; }
  }
  return std::nullopt;
}

std::optional<DriveChoice> StorageModel::ChooseDrive(
    const std::string& volume) const
{
  std::lock_guard lock(mutex_);

  // A volume can only be in one drive; if it is loaded, that drive is the
  // only valid choice, even if it is momentarily unusable.
  for (const auto& drive : drives_) {
    if (drive.loaded_volume == volume) {
      DriveFacts facts{.enabled = drive.enabled,
                       .pending = drive.pending,
                       .in_use = !drive.jobs.empty(),
                       .has_volume = true,
                       .holds_requested_volume = true};
      auto kind = ClassifyDrive(facts);
      if (kind == DriveCandidate::kUnusable) { return std::nullopt; }
      return DriveChoice{drive.name, kind};
    }
  }

  std::optional<DriveChoice> best;
  for (const auto& drive : drives_) {
    DriveFacts facts{.enabled = drive.enabled,
                     .pending = drive.pending,
                     .in_use = !drive.jobs.empty(),
                     .has_volume = drive.loaded_volume.has_value(),
                     .holds_requested_volume = false};
    auto kind = ClassifyDrive(facts);
    if (kind == DriveCandidate::kUnusable) { continue; }
    if (!best || Rank(kind) > Rank(best->kind)) {
      best = DriveChoice{drive.name, kind};
    }
  }
  return best;
}

LeaseResult StorageModel::AcquireLease(JobId job,
                                       const std::string& volume,
                                       AccessMode mode,
                                       const std::string& drive_name)
{
  std::lock_guard lock(mutex_);
  DriveState* drive = FindDrive(drive_name);
  if (!drive) { return LeaseResult::kUnknownDrive; }

  auto [first, last] = leases_.equal_range(volume);
  for (auto it = first; it != last; ++it) {
    const VolumeLease& held = it->second;
    if (held.job == job) {
      return held.drive == drive_name ? LeaseResult::kAlreadyHeld
                                      : LeaseResult::kConflict;
    }
    if (held.drive != drive_name || LeasesConflict(held.mode, mode)) {
      return LeaseResult::kConflict;
    }
  }

  // The drive may only be shared by jobs using the same volume.
  if (drive->loaded_volume && *drive->loaded_volume != volume
      && !drive->jobs.empty()) {
    return LeaseResult::kDriveBusy;
  }
  for (const auto& [other_volume, lease] : leases_) {
    if (lease.drive == drive_name && other_volume != volume) {
      return LeaseResult::kDriveBusy;
    }
  }

  leases_.emplace(volume, VolumeLease{job, mode, drive_name});
  if (std::find(drive->jobs.begin(), drive->jobs.end(), job)
      == drive->jobs.end()) {
    drive->jobs.push_back(job);
  }
  return LeaseResult::kGranted;
}

void StorageModel::ReleaseLeases(JobId job)
{
  std::lock_guard lock(mutex_);
  std::erase_if(leases_, [&](const auto& kv) { return kv.second.job == job; });
  for (auto& drive : drives_) { std::erase(drive.jobs, job); }
}

std::vector<VolumeLease> StorageModel::LeasesOf(const std::string& volume) const
{
  std::lock_guard lock(mutex_);
  std::vector<VolumeLease> result;
  auto [first, last] = leases_.equal_range(volume);
  for (auto it = first; it != last; ++it) { result.push_back(it->second); }
  return result;
}

bool StorageModel::IsLeasedByOther(const std::string& volume, JobId job) const
{
  std::lock_guard lock(mutex_);
  auto [first, last] = leases_.equal_range(volume);
  return std::any_of(first, last,
                     [&](const auto& kv) { return kv.second.job != job; });
}

void StorageModel::MarkPending(const std::string& drive_name, bool pending)
{
  std::lock_guard lock(mutex_);
  if (DriveState* drive = FindDrive(drive_name)) { drive->pending = pending; }
}

bool StorageModel::VolumeLoaded(const std::string& drive_name,
                                const std::string& volume,
                                std::optional<int> from_slot)
{
  std::lock_guard lock(mutex_);
  DriveState* drive = FindDrive(drive_name);
  if (!drive) { return false; }
  drive->loaded_volume = volume;
  drive->loaded_from_slot = from_slot;
  drive->pending = false;
  for (auto& slot : slots_) {
    if (slot.volume == volume) { slot.volume.reset(); }
  }
  return true;
}

bool StorageModel::VolumeUnloaded(const std::string& drive_name)
{
  std::lock_guard lock(mutex_);
  DriveState* drive = FindDrive(drive_name);
  if (!drive) { return false; }
  if (drive->loaded_volume && drive->loaded_from_slot) {
    auto it = std::find_if(slots_.begin(), slots_.end(), [&](const auto& s) {
      return s.slot == *drive->loaded_from_slot;
    });
    if (it != slots_.end()) {
      it->volume = drive->loaded_volume;
    } else {
      slots_.push_back({*drive->loaded_from_slot, drive->loaded_volume});
    }
  }
  drive->loaded_volume.reset();
  drive->loaded_from_slot.reset();
  drive->pending = false;
  return true;
}

std::vector<Divergence> StorageModel::FindDivergences(
    const std::vector<CatalogVolumeLocation>& catalog) const
{
  std::lock_guard lock(mutex_);

  std::map<std::string, std::optional<int>> actual;
  for (const auto& slot : slots_) {
    if (slot.volume) { actual[*slot.volume] = slot.slot; }
  }
  for (const auto& drive : drives_) {
    if (drive.loaded_volume) {
      actual[*drive.loaded_volume] = drive.loaded_from_slot;
    }
  }

  std::vector<Divergence> result;
  std::set<std::string> seen;
  for (const auto& entry : catalog) {
    seen.insert(entry.volume);
    std::optional<int> catalog_slot;
    if (entry.in_changer) { catalog_slot = entry.slot; }
    auto it = actual.find(entry.volume);
    std::optional<int> actual_slot;
    bool present = it != actual.end();
    if (present) { actual_slot = it->second; }

    if (catalog_slot.has_value() != present
        || (present && actual_slot && catalog_slot != actual_slot)) {
      result.push_back({entry.volume, catalog_slot, present, actual_slot});
    }
  }
  for (const auto& [volume, slot] : actual) {
    if (!seen.contains(volume)) {
      result.push_back({volume, std::nullopt, true, slot});
    }
  }
  return result;
}

std::string DriveName(int drive_index)
{
  return "Drive-" + std::to_string(drive_index);
}

Inventory BuildInventory(const std::vector<ChangerElement>& elements)
{
  Inventory inventory;
  for (const auto& element : elements) {
    switch (element.type) {
      case ChangerElement::Type::kDrive: {
        DriveState drive;
        drive.name = DriveName(element.number);
        drive.loaded_volume = element.volume;
        if (element.volume) { drive.loaded_from_slot = element.loaded_slot; }
        inventory.drives.push_back(std::move(drive));
        break;
      }
      case ChangerElement::Type::kSlot:
      case ChangerElement::Type::kImportExport:
        inventory.slots.push_back({element.number, element.volume});
        break;
    }
  }
  std::sort(inventory.slots.begin(), inventory.slots.end(),
            [](const auto& a, const auto& b) { return a.slot < b.slot; });
  return inventory;
}

static std::string ModeName(AccessMode mode)
{
  return mode == AccessMode::kAppend ? "append" : "read";
}

std::vector<std::string> DescribeModel(const StorageModel& model)
{
  std::vector<std::string> lines;
  auto drives = model.Drives();
  lines.push_back("Storage \"" + model.StorageName()
                  + "\": " + std::to_string(drives.size()) + " drive(s)");
  for (const auto& drive : drives) {
    std::string line = "  " + drive.name + ": ";
    if (drive.loaded_volume) {
      line += "volume \"" + *drive.loaded_volume + "\"";
      if (drive.loaded_from_slot) {
        line += " from slot " + std::to_string(*drive.loaded_from_slot);
      }
    } else {
      line += "empty";
    }
    if (!drive.enabled) { line += ", disabled"; }
    if (drive.pending) { line += ", command pending"; }
    for (const auto& lease : model.LeasesOf(drive.loaded_volume.value_or(""))) {
      line += ", JobId " + std::to_string(lease.job) + " ("
              + ModeName(lease.mode) + ")";
    }
    lines.push_back(std::move(line));
  }
  for (const auto& slot : model.Slots()) {
    lines.push_back("  Slot " + std::to_string(slot.slot) + ": "
                    + (slot.volume ? "\"" + *slot.volume + "\"" : "empty"));
  }
  return lines;
}

std::string DescribeDivergence(const Divergence& divergence)
{
  auto where = [](bool present, const std::optional<int>& slot) {
    if (!present) { return std::string("not present"); }
    return slot ? "slot " + std::to_string(*slot) : std::string("unknown slot");
  };
  return "Volume \"" + divergence.volume + "\": catalog "
         + where(divergence.catalog_slot.has_value(), divergence.catalog_slot)
         + ", storage daemon "
         + where(divergence.in_storage, divergence.actual_slot);
}

std::shared_ptr<StorageModel> StorageManager::Get(
    const std::string& storage_name)
{
  std::lock_guard lock(mutex_);
  auto& model = models_[storage_name];
  if (!model) { model = std::make_shared<StorageModel>(storage_name); }
  return model;
}

std::shared_ptr<StorageModel> StorageManager::Find(
    const std::string& storage_name) const
{
  std::lock_guard lock(mutex_);
  auto it = models_.find(storage_name);
  return it == models_.end() ? nullptr : it->second;
}

void StorageManager::Remove(const std::string& storage_name)
{
  std::lock_guard lock(mutex_);
  models_.erase(storage_name);
}

std::vector<std::string> StorageManager::StorageNames() const
{
  std::lock_guard lock(mutex_);
  std::vector<std::string> names;
  for (const auto& [name, model] : models_) { names.push_back(name); }
  return names;
}

StorageManager& GlobalStorageManager()
{
  static StorageManager manager;
  return manager;
}

}  // namespace directordaemon::storage_manager
