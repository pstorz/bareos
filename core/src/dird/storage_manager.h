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

/*
 * Director side model of the drives, autochanger slots and volume leases of
 * one storage daemon. This is the foundation for moving all volume and
 * device decisions into the director (see docs/adr/0007-*.md). The model is
 * pure state and policy: it does no network or catalog I/O, so every
 * decision can be made in a single critical section and unit tested.
 */

#ifndef BAREOS_DIRD_STORAGE_MANAGER_H_
#define BAREOS_DIRD_STORAGE_MANAGER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "include/dir_sd_protocol.h"
#include "lib/device_inventory.h"

namespace directordaemon::storage_manager {

using JobId = std::uint32_t;

enum class AccessMode
{
  kRead,
  kAppend,
};

/* A volume is bound to one drive. Several jobs may use it there at the
 * same time (concurrent appends or concurrent reads), but a drive cannot
 * read and append the same volume at once. */
constexpr bool LeasesConflict(AccessMode held, AccessMode requested)
{
  return held != requested;
}

enum class DriveCandidate
{
  kUnusable,      // busy with another volume or disabled
  kNeedsUnload,   // idle, but holds another volume
  kEmpty,         // idle and empty, needs a load
  kVolumeLoaded,  // already holds the requested volume
};

// Higher rank = better choice for mounting a volume.
constexpr int Rank(DriveCandidate c) { return static_cast<int>(c); }

struct DriveFacts {
  bool enabled{true};
  bool pending{false};  // a load/unload command is in flight
  bool in_use{false};   // a job currently uses the drive
  bool has_volume{false};
  bool holds_requested_volume{false};
};

constexpr DriveCandidate ClassifyDrive(const DriveFacts& f)
{
  if (!f.enabled || f.pending) { return DriveCandidate::kUnusable; }
  if (f.holds_requested_volume) { return DriveCandidate::kVolumeLoaded; }
  if (f.in_use) { return DriveCandidate::kUnusable; }
  if (f.has_volume) { return DriveCandidate::kNeedsUnload; }
  return DriveCandidate::kEmpty;
}

struct DriveState {
  std::string name;
  std::string device;  // storage daemon device, once known
  bool changer{true};  // false: standalone device, not in the inventory
  bool enabled{true};
  bool pending{false};
  std::optional<std::string> loaded_volume;
  std::optional<int> loaded_from_slot;
  std::vector<JobId> jobs;
  std::optional<DeviceInventoryEntry> sd;  // last report of the SD
};

struct SlotState {
  int slot{};
  std::optional<std::string> volume;
};

struct VolumeLease {
  JobId job{};
  AccessMode mode{AccessMode::kRead};
  std::string drive;
};

// Catalog view of one volume, used to detect divergences from the SD.
struct CatalogVolumeLocation {
  std::string volume;
  int slot{};
  bool in_changer{false};
};

struct Divergence {
  std::string volume;
  std::optional<int> catalog_slot;  // nullopt: not in changer per catalog
  bool in_storage{false};           // present in a slot or drive per SD
  std::optional<int> actual_slot;   // home slot per SD, if known
};

enum class LeaseResult
{
  kGranted,
  kAlreadyHeld,
  kConflict,
  kUnknownDrive,
};

/* Slot the catalog must be corrected to, given where the storage model
 * knows the volume to be; nullopt if the catalog is right or the location
 * is unknown. */
struct CatalogSlot {
  int slot{};
  bool in_changer{false};
  bool same_storage{true};
};

constexpr std::optional<int> CorrectedSlot(const CatalogSlot& catalog,
                                           std::optional<int> actual)
{
  if (!actual || *actual <= 0) { return std::nullopt; }
  if (catalog.in_changer && catalog.slot == *actual && catalog.same_storage) {
    return std::nullopt;
  }
  return actual;
}

enum class CatalogFixKind
{
  kNone,
  kSetSlot,       // the volume is in the changer, in this slot
  kNotInChanger,  // the catalog places the volume in the changer wrongly
};

struct CatalogFix {
  CatalogFixKind kind{CatalogFixKind::kNone};
  int slot{};
  bool operator==(const CatalogFix&) const = default;
};

/* How to correct the catalog for a volume whose catalog location differs
 * from a fresh changer listing. A volume known to be in the changer at
 * an unknown slot is left alone. */
constexpr CatalogFix CatalogFixFor(std::optional<int> catalog_slot,
                                   bool in_storage,
                                   std::optional<int> actual_slot)
{
  if (in_storage) {
    if (!actual_slot || *actual_slot <= 0 || catalog_slot == actual_slot) {
      return {};
    }
    return {CatalogFixKind::kSetSlot, *actual_slot};
  }
  if (catalog_slot) { return {CatalogFixKind::kNotInChanger, 0}; }
  return {};
}

/* Drive index of an autochanger drive of the model ("Drive-<n>"). */
constexpr std::optional<int> DriveIndexOf(std::string_view drive)
{
  constexpr std::string_view prefix = "Drive-";
  if (!drive.starts_with(prefix) || drive.size() == prefix.size()) {
    return std::nullopt;
  }
  int index = 0;
  for (char c : drive.substr(prefix.size())) {
    if (c < '0' || c > '9' || index > 100000) { return std::nullopt; }
    index = index * 10 + (c - '0');
  }
  return index;
}

// Where the storage model knows a volume to be, seen from one drive.
struct VolumeWhereabouts {
  bool known{false};          // the model holds a current changer listing
  bool changer_drive{false};  // the requesting drive is an autochanger drive
  bool in_requesting_drive{false};
  std::optional<int> other_drive;  // index of another drive holding it
  bool other_drive_busy{false};    // another job uses that drive
  bool in_slot{false};
};

/* How the requesting drive gets the volume. Without a current listing
 * or outside of an autochanger the storage daemon decides as before. */
constexpr ChangerInstruction PlanChange(const VolumeWhereabouts& w)
{
  if (!w.known || !w.changer_drive) { return {}; }
  if (w.in_requesting_drive) { return {ChangerAction::kMounted, -1}; }
  if (w.other_drive) {
    return {
        w.other_drive_busy ? ChangerAction::kBusy : ChangerAction::kTransfer,
        *w.other_drive};
  }
  if (w.in_slot) { return {ChangerAction::kLoad, -1}; }
  return {};
}

struct DriveChoice {
  std::string drive;
  DriveCandidate kind{DriveCandidate::kUnusable};
};

class StorageModel {
 public:
  explicit StorageModel(std::string storage_name)
      : storage_name_(std::move(storage_name))
  {
  }

  const std::string& StorageName() const { return storage_name_; }

  // Replace drive and slot state with an SD inventory. Drive job
  // assignments and existing leases are kept for drives that still exist.
  void SetInventory(std::vector<DriveState> drives,
                    std::vector<SlotState> slots);

  std::vector<DriveState> Drives() const;
  std::optional<std::string> LoadedVolume(const std::string& drive) const;
  std::vector<SlotState> Slots() const;
  std::optional<int> SlotOfVolume(const std::string& volume) const;
  /* Like SlotOfVolume, but only while the slot inventory is known to be
   * current: since the last changer listing only jobs moved volumes, and
   * they report each move. */
  std::optional<int> KnownSlotOfVolume(const std::string& volume) const;
  // Operator commands moved volumes without telling the model.
  void InvalidateSlots();
  bool SlotsKnown() const;
  /* Only one caller refreshes outdated slots at a time: true if the
   * caller must list the changer and then call EndSlotRefresh(). */
  bool BeginSlotRefresh();
  void EndSlotRefresh();
  /* Remember the start time of the storage daemon. A restarted storage
   * daemon may have found other volumes in the changer, so the slots
   * become unknown. Returns true if a restart was detected. */
  bool NoteStorageDaemonEpoch(const std::string& epoch);

  /* Decide how a drive gets a volume for a job, see PlanChange(). The
   * drive holding it is busy if another job holds a lease there. */
  ChangerInstruction PlanVolumeChange(const std::string& drive,
                                      const std::string& volume,
                                      JobId job) const;

  // Pick the best drive to use for a volume; nullopt if none is usable.
  std::optional<DriveChoice> ChooseDrive(const std::string& volume) const;

  // Atomically lease a volume on a drive for a job.
  LeaseResult AcquireLease(JobId job,
                           const std::string& volume,
                           AccessMode mode,
                           const std::string& drive);
  void ReleaseLeases(JobId job);
  std::vector<VolumeLease> LeasesOf(const std::string& volume) const;
  std::multimap<std::string, VolumeLease> Leases() const;
  bool IsLeasedByOther(const std::string& volume, JobId job) const;

  // Record the result of executed SD commands.
  /* Map a storage daemon device to a drive of the model: autochanger
   * drives by their index, other devices by name (added on first use).
   * Returns the drive name. */
  std::string BindDevice(const std::string& device,
                         std::optional<int> drive_index);

  /* Apply the device inventory of the storage daemon: binds devices to
   * drives and updates loaded volumes and usage. */
  void UpdateDevices(const std::vector<DeviceInventoryEntry>& devices);

  void MarkPending(const std::string& drive, bool pending);
  bool VolumeLoaded(const std::string& drive,
                    const std::string& volume,
                    std::optional<int> from_slot);
  bool VolumeUnloaded(const std::string& drive,
                      std::optional<int> to_slot = std::nullopt);
  /* The storage daemon loaded a slot into a drive. The volume is taken
   * from the slot inventory, the reported volume is used if the slot
   * content is unknown. Returns the volume now in the drive. */
  std::optional<std::string> SlotLoaded(const std::string& drive,
                                        int slot,
                                        const std::string& reported_volume);
  bool HasDevice(const std::string& device) const;

  std::vector<Divergence> FindDivergences(
      const std::vector<CatalogVolumeLocation>& catalog) const;

 private:
  void ReturnToSlot(DriveState& drive,
                    std::optional<int> to_slot = std::nullopt);
  DriveState* FindDrive(const std::string& name);
  const DriveState* FindDrive(const std::string& name) const;

  mutable std::mutex mutex_;
  std::string storage_name_;
  std::vector<DriveState> drives_;
  std::vector<SlotState> slots_;
  bool slots_valid_{false};
  bool slot_refresh_running_{false};
  std::optional<std::string> sd_epoch_;
  std::multimap<std::string, VolumeLease> leases_;
};

// One element as reported by "autochanger listall" of the storage daemon.
struct ChangerElement {
  enum class Type
  {
    kDrive,
    kSlot,
    kImportExport,
  };
  Type type{Type::kSlot};
  int number{};                    // drive index or slot number
  std::optional<int> loaded_slot;  // only for drives
  std::optional<std::string> volume;
};

struct Inventory {
  std::vector<DriveState> drives;
  std::vector<SlotState> slots;
};

std::string DriveName(int drive_index);

/* Facts about one device, from the point of view of a job that wants
 * to reserve it. */
struct DeviceUse {
  bool append_request{true};
  bool prefer_mounted{false};
  bool unavailable{false};  // unmounted by the operator
  bool reading{false};
  bool has_volume{false};
  bool same_pool{false};  // reserved for or holding the job's pool
  int users{0};           // writers and reservations
  // position of the loaded volume in the volumes a job reads, -1 if none
  int read_rank{-1};
};

/* Preference of a device for a job, lower is better. Without "Prefer
 * Mounted Volumes" a free drive is best, so jobs spread over the drives;
 * with it, a drive already appending to the job's pool is best. */
constexpr int DevicePreference(const DeviceUse& d)
{
  if (d.unavailable) { return 9; }
  if (!d.append_request) {
    // a free drive holding the first volume to read needs no load at all,
    // an empty one only a load, any other one an unload first
    if (d.users > 0 || d.reading) { return 6; }
    if (d.read_rank == 0) { return 0; }
    if (d.read_rank > 0) { return 1; }
    return d.has_volume ? 3 : 2;
  }
  if (d.reading) { return 8; }
  if (d.prefer_mounted) {
    if (d.users > 0) { return d.same_pool ? 0 : 7; }
    if (d.has_volume) { return d.same_pool ? 1 : 3; }
    return 2;
  }
  if (d.users == 0) {
    if (!d.has_volume) { return 0; }
    return d.same_pool ? 1 : 2;
  }
  return d.same_pool ? 3 : 7;
}

struct DeviceRequest {
  bool append{true};
  bool prefer_mounted{false};
  std::string pool;
  std::string media_type;
  std::vector<std::string> read_volumes;  // in the order the job reads them
};

/* The volumes a bootstrap refers to, in the order of their first use.
 * Volume lines look like Volume="A" or Volume="A|B". */
constexpr std::vector<std::string> BootstrapVolumes(std::string_view text)
{
  constexpr std::string_view key = "Volume=";
  std::vector<std::string> volumes;
  while (!text.empty()) {
    auto end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{}
                                         : text.substr(end + 1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    while (
        !line.empty()
        && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    if (!line.starts_with(key)) { continue; }
    line.remove_prefix(key.size());
    if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
      line = line.substr(1, line.size() - 2);
    }
    while (!line.empty()) {
      auto bar = line.find('|');
      std::string name{line.substr(0, bar)};
      line = bar == std::string_view::npos ? std::string_view{}
                                           : line.substr(bar + 1);
      if (name.empty()) { continue; }
      bool known = false;
      for (const auto& v : volumes) { known = known || v == name; }
      if (!known) { volumes.push_back(std::move(name)); }
    }
  }
  return volumes;
}

/* The drives of a director storage that can serve the request, best
 * first. Autochangers are expanded into their autoselect drives. An
 * empty result means the inventory does not know the storage. */
std::vector<std::string> OrderDevices(
    const std::vector<DeviceInventoryEntry>& inventory,
    const std::vector<std::string>& storage_devices,
    const DeviceRequest& request);

/* The devices of an inventory that belong to a director storage, given
 * the device names configured there (autochangers or single devices). */
std::vector<DeviceInventoryEntry> DevicesOfStorage(
    const std::vector<DeviceInventoryEntry>& inventory,
    const std::vector<std::string>& storage_devices);
Inventory BuildInventory(const std::vector<ChangerElement>& elements);

// Human readable dump of the model, one line per entry.
std::vector<std::string> DescribeModel(const StorageModel& model);
std::string DescribeDivergence(const Divergence& divergence);

// Process wide registry: one model per storage resource name.
class StorageManager {
 public:
  std::shared_ptr<StorageModel> Get(const std::string& storage_name);
  std::shared_ptr<StorageModel> Find(const std::string& storage_name) const;
  void Remove(const std::string& storage_name);
  std::vector<std::string> StorageNames() const;
  void ReleaseLeases(JobId job);

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<StorageModel>> models_;
};

StorageManager& GlobalStorageManager();

}  // namespace directordaemon::storage_manager

#endif  // BAREOS_DIRD_STORAGE_MANAGER_H_
