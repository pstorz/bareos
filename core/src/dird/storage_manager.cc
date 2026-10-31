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
static_assert(!LeasesConflict(AccessMode::kAppend, AccessMode::kAppend));

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
      drive.device = old->device;
      drive.sd = old->sd;
    }
  }
  for (auto& old : drives_) {
    if (!old.changer) { drives.push_back(std::move(old)); }
  }
  drives_ = std::move(drives);
  slots_ = std::move(slots);
  slots_valid_ = true;

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

std::optional<std::string> StorageModel::LoadedVolume(
    const std::string& drive_name) const
{
  std::lock_guard lock(mutex_);
  const DriveState* drive = FindDrive(drive_name);
  if (!drive) { return std::nullopt; }
  return drive->loaded_volume;
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

std::optional<int> StorageModel::KnownSlotOfVolume(
    const std::string& volume) const
{
  std::lock_guard lock(mutex_);
  if (!slots_valid_) { return std::nullopt; }
  for (const auto& slot : slots_) {
    if (slot.volume == volume) { return slot.slot; }
  }
  for (const auto& drive : drives_) {
    if (drive.loaded_volume == volume) { return drive.loaded_from_slot; }
  }
  return std::nullopt;
}

void StorageModel::InvalidateSlots()
{
  std::lock_guard lock(mutex_);
  slots_valid_ = false;
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
    if (!drive.changer) { continue; }
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
    if (held.job == job && held.mode == mode && held.drive == drive_name) {
      return LeaseResult::kAlreadyHeld;
    }
    if (held.job == job) { continue; }
    if (held.drive != drive_name || LeasesConflict(held.mode, mode)) {
      return LeaseResult::kConflict;
    }
  }

  // A job uses one volume per mode; a new one replaces the previous one.
  std::erase_if(leases_, [&](const auto& kv) {
    return kv.second.job == job && kv.second.mode == mode;
  });
  for (auto& other : drives_) {
    bool still_used
        = std::any_of(leases_.begin(), leases_.end(), [&](const auto& kv) {
            return kv.second.job == job && kv.second.drive == other.name;
          });
    if (!still_used) { std::erase(other.jobs, job); }
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

std::multimap<std::string, VolumeLease> StorageModel::Leases() const
{
  std::lock_guard lock(mutex_);
  return leases_;
}

std::string StorageModel::BindDevice(const std::string& device,
                                     std::optional<int> drive_index)
{
  std::lock_guard lock(mutex_);
  if (drive_index) {
    DriveState* drive = FindDrive(DriveName(*drive_index));
    if (drive && drive->changer
        && (drive->device.empty() || drive->device == device)) {
      drive->device = device;
      return drive->name;
    }
  }
  for (const auto& drive : drives_) {
    if (drive.device == device) { return drive.name; }
  }
  DriveState drive;
  drive.name = device;
  drive.device = device;
  drive.changer = false;
  drives_.push_back(std::move(drive));
  return device;
}

bool StorageModel::IsLeasedByOther(const std::string& volume, JobId job) const
{
  std::lock_guard lock(mutex_);
  auto [first, last] = leases_.equal_range(volume);
  return std::any_of(first, last,
                     [&](const auto& kv) { return kv.second.job != job; });
}

void StorageModel::UpdateDevices(
    const std::vector<DeviceInventoryEntry>& devices)
{
  std::lock_guard lock(mutex_);
  for (const auto& entry : devices) {
    const bool changer = !entry.changer.empty() && entry.drive_index >= 0;
    std::string name = changer ? DriveName(entry.drive_index) : entry.device;
    DriveState* drive = FindDrive(name);
    if (!drive) {
      DriveState added;
      added.name = name;
      added.changer = changer;
      drives_.push_back(std::move(added));
      drive = &drives_.back();
    }
    drive->device = entry.device;
    drive->enabled = !entry.unmounted;
    /* For changer drives the changer inventory knows what is loaded even
     * if the SD did not read the label yet, so only add knowledge. */
    if (!entry.volume.empty()) {
      drive->loaded_volume = entry.volume;
      if (entry.slot > 0) { drive->loaded_from_slot = entry.slot; }
    } else if (!changer) {
      drive->loaded_volume.reset();
      drive->loaded_from_slot.reset();
    }
    drive->sd = entry;
  }
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

void StorageModel::ReturnToSlot(DriveState& drive, std::optional<int> to_slot)
{
  auto slot = drive.loaded_from_slot ? drive.loaded_from_slot : to_slot;
  if (drive.loaded_volume && slot) {
    auto it = std::find_if(slots_.begin(), slots_.end(),
                           [&](const auto& s) { return s.slot == *slot; });
    if (it != slots_.end()) {
      it->volume = drive.loaded_volume;
    } else {
      slots_.push_back({*slot, drive.loaded_volume});
    }
  }
  drive.loaded_volume.reset();
  drive.loaded_from_slot.reset();
  drive.pending = false;
}

bool StorageModel::VolumeUnloaded(const std::string& drive_name,
                                  std::optional<int> to_slot)
{
  std::lock_guard lock(mutex_);
  DriveState* drive = FindDrive(drive_name);
  if (!drive) { return false; }
  ReturnToSlot(*drive, to_slot);
  return true;
}

std::optional<std::string> StorageModel::SlotLoaded(
    const std::string& drive_name,
    int slot_number,
    const std::string& reported_volume)
{
  std::lock_guard lock(mutex_);
  DriveState* drive = FindDrive(drive_name);
  if (!drive) { return std::nullopt; }
  if (drive->loaded_volume && drive->loaded_from_slot != slot_number) {
    ReturnToSlot(*drive);
  }
  std::optional<std::string> volume;
  for (auto& slot : slots_) {
    if (slot.slot == slot_number && slot.volume) {
      volume = std::move(slot.volume);
      slot.volume.reset();
    }
  }
  if (!volume && drive->loaded_from_slot == slot_number) {
    volume = drive->loaded_volume;
  }
  if (!volume && !reported_volume.empty()) { volume = reported_volume; }
  if (volume) {
    for (auto& slot : slots_) {
      if (slot.volume == volume) { slot.volume.reset(); }
    }
    for (auto& other : drives_) {
      if (&other != drive && other.loaded_volume == volume) {
        other.loaded_volume.reset();
        other.loaded_from_slot.reset();
      }
    }
  }
  drive->loaded_volume = volume;
  drive->loaded_from_slot = slot_number;
  drive->pending = false;
  return volume;
}

bool StorageModel::HasDevice(const std::string& device) const
{
  std::lock_guard lock(mutex_);
  return std::any_of(drives_.begin(), drives_.end(),
                     [&](const auto& d) { return d.device == device; });
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

std::vector<DeviceInventoryEntry> DevicesOfStorage(
    const std::vector<DeviceInventoryEntry>& inventory,
    const std::vector<std::string>& storage_devices)
{
  std::vector<DeviceInventoryEntry> result;
  for (const auto& entry : inventory) {
    for (const auto& name : storage_devices) {
      if (name == entry.device
          || (!entry.changer.empty() && name == entry.changer)) {
        result.push_back(entry);
        break;
      }
    }
  }
  return result;
}

std::vector<std::string> OrderDevices(
    const std::vector<DeviceInventoryEntry>& inventory,
    const std::vector<std::string>& storage_devices,
    const DeviceRequest& request)
{
  struct Candidate {
    int preference;
    int users;
    std::string device;
  };
  std::vector<Candidate> candidates;
  auto add = [&](const DeviceInventoryEntry& entry) {
    if (request.append ? entry.read_only : entry.write_only) { return; }
    if (!request.media_type.empty() && entry.media_type != request.media_type) {
      return;
    }
    for (const auto& c : candidates) {
      if (c.device == entry.device) { return; }
    }
    DeviceUse use{
        .append_request = request.append,
        .prefer_mounted = request.prefer_mounted,
        .unavailable = entry.unmounted,
        .reading = entry.reading,
        .has_volume = !entry.volume.empty(),
        .same_pool = !entry.pool.empty() && entry.pool == request.pool,
        .users = entry.writers + entry.reserved,
    };
    candidates.push_back({DevicePreference(use), use.users, entry.device});
  };

  for (const auto& name : storage_devices) {
    for (const auto& entry : inventory) {
      if (entry.device == name) {
        add(entry);
      } else if (!entry.changer.empty() && entry.changer == name
                 && entry.autoselect) {
        add(entry);
      }
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     if (a.preference != b.preference) {
                       return a.preference < b.preference;
                     }
                     return a.users < b.users;
                   });
  std::vector<std::string> ordered;
  for (auto& c : candidates) { ordered.push_back(std::move(c.device)); }
  return ordered;
}

std::vector<std::string> DescribeModel(const StorageModel& model)
{
  std::vector<std::string> lines;
  auto drives = model.Drives();
  auto leases = model.Leases();
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
    if (drive.sd) {
      if (drive.sd->reading) { line += ", reading"; }
      if (drive.sd->appending) { line += ", appending"; }
      if (drive.sd->writers) {
        line += ", " + std::to_string(drive.sd->writers) + " writer(s)";
      }
      if (drive.sd->reserved) {
        line += ", " + std::to_string(drive.sd->reserved) + " reservation(s)";
      }
      if (drive.sd->blocked && !drive.sd->unmounted) { line += ", blocked"; }
    }
    if (!drive.device.empty() && drive.device != drive.name) {
      line += ", device \"" + drive.device + "\"";
    }
    for (const auto& [volume, lease] : leases) {
      if (lease.drive != drive.name) { continue; }
      line += ", JobId " + std::to_string(lease.job) + " ("
              + ModeName(lease.mode) + " \"" + volume + "\")";
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

void StorageManager::ReleaseLeases(JobId job)
{
  std::lock_guard lock(mutex_);
  for (auto& [name, model] : models_) { model->ReleaseLeases(job); }
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
