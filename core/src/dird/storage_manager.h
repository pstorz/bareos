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
#include <vector>

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
  bool enabled{true};
  bool pending{false};
  std::optional<std::string> loaded_volume;
  std::optional<int> loaded_from_slot;
  std::vector<JobId> jobs;
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
  std::vector<SlotState> Slots() const;
  std::optional<int> SlotOfVolume(const std::string& volume) const;

  // Pick the best drive to use for a volume; nullopt if none is usable.
  std::optional<DriveChoice> ChooseDrive(const std::string& volume) const;

  // Atomically lease a volume on a drive for a job.
  LeaseResult AcquireLease(JobId job,
                           const std::string& volume,
                           AccessMode mode,
                           const std::string& drive);
  void ReleaseLeases(JobId job);
  std::vector<VolumeLease> LeasesOf(const std::string& volume) const;
  bool IsLeasedByOther(const std::string& volume, JobId job) const;

  // Record the result of executed SD commands.
  void MarkPending(const std::string& drive, bool pending);
  bool VolumeLoaded(const std::string& drive,
                    const std::string& volume,
                    std::optional<int> from_slot);
  bool VolumeUnloaded(const std::string& drive);

  std::vector<Divergence> FindDivergences(
      const std::vector<CatalogVolumeLocation>& catalog) const;

 private:
  DriveState* FindDrive(const std::string& name);
  const DriveState* FindDrive(const std::string& name) const;

  mutable std::mutex mutex_;
  std::string storage_name_;
  std::vector<DriveState> drives_;
  std::vector<SlotState> slots_;
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

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<StorageModel>> models_;
};

StorageManager& GlobalStorageManager();

}  // namespace directordaemon::storage_manager

#endif  // BAREOS_DIRD_STORAGE_MANAGER_H_
