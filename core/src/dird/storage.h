/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2026 Bareos GmbH & Co. KG

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

#ifndef BAREOS_DIRD_STORAGE_H_
#define BAREOS_DIRD_STORAGE_H_

#include "ndmp/smc.h"
#include "dird/dird.h"

#include <optional>
#include <vector>

#include "lib/device_inventory.h"

template <typename T> class alist;

namespace directordaemon {

void CopyRwstorage(JobControlRecord* jcr,
                   alist<StorageResource*>* storage,
                   const char* where);
void SetRwstorage(JobControlRecord* jcr, UnifiedStorageResource* store);
void FreeRwstorage(JobControlRecord* jcr);
void CopyWstorage(JobControlRecord* jcr,
                  alist<StorageResource*>* storage,
                  const char* where);
void SetWstorage(JobControlRecord* jcr, UnifiedStorageResource* store);
void FreeWstorage(JobControlRecord* jcr);
void CopyRstorage(JobControlRecord* jcr,
                  alist<StorageResource*>* storage,
                  const char* where);
void SetRstorage(JobControlRecord* jcr, UnifiedStorageResource* store);
void FreeRstorage(JobControlRecord* jcr);
void SetPairedStorage(JobControlRecord* jcr);
void FreePairedStorage(JobControlRecord* jcr);
bool HasPairedStorage(JobControlRecord* jcr);
bool SelectNextRstore(JobControlRecord* jcr, bootstrap_info& info);
void StorageStatus(UaContext* ua, StorageResource* store, char* cmd);
int StorageCompareVolListEntry(vol_list_t* e1, vol_list_t* e2);
changer_vol_list_t* get_vol_list_from_storage(UaContext* ua,
                                              StorageResource* store,
                                              bool listall,
                                              bool scan,
                                              bool cached = true);
slot_number_t GetNumSlots(UaContext* ua, StorageResource* store);
drive_number_t GetNumDrives(UaContext* ua, StorageResource* store);
bool transfer_volume(UaContext* ua,
                     StorageResource* store,
                     slot_number_t src_slot,
                     slot_number_t dst_slot);
bool DoAutochangerVolumeOperation(UaContext* ua,
                                  StorageResource* store,
                                  const char* operation,
                                  drive_number_t drive,
                                  slot_number_t slot);
vol_list_t* vol_is_loaded_in_drive(StorageResource* store,
                                   changer_vol_list_t* vol_list,
                                   slot_number_t slot);
void StorageReleaseVolList(StorageResource* store,
                           changer_vol_list_t* vol_list);
void StorageFreeVolList(StorageResource* store, changer_vol_list_t* vol_list);
void InvalidateVolList(StorageResource* store);
/* Volumes were moved by an operator command, the slot inventory of the
 * storage model is outdated until the next changer listing. */
void InvalidateStorageModel(StorageResource* store);
int CompareStorageMapping(void* e1, void* e2);

slot_number_t GetElementAddressByBareosSlotNumber(
    smc_element_address_assignment* smc_elem_aa,
    slot_type_t slot_type,
    slot_number_t slotnumber);
slot_number_t GetBareosSlotNumberByElementAddress(
    smc_element_address_assignment* smc_elem_aa,
    slot_type_t slot_type,
    slot_number_t element_addr);
bool IsSameStorageDaemon(StorageResource* read_storage,
                         StorageResource* write_storage);
/* Ask the storage daemon for the state of its devices; nullopt if it
 * did not answer with an inventory. */
std::optional<std::vector<DeviceInventoryEntry>> QueryDeviceInventory(
    BareosSocket* sd,
    std::string* epoch = nullptr);
/* Update the storage model of the given storages from an inventory and
 * the epoch of the storage daemon that sent it. */
void ApplyDeviceInventory(const std::vector<DeviceInventoryEntry>& inventory,
                          const std::vector<StorageResource*>& stores,
                          const std::string& epoch = {});
/* Make sure the storage model knows the slots of an autochanger: list
 * the changer if they are unknown (director start, operator commands,
 * storage daemon restart) and correct Slot and InChanger in the catalog
 * from the listing. */
void EnsureSlotInventory(JobControlRecord* jcr, StorageResource* store);
// Query the inventory and apply it; false if it could not be queried.
bool RefreshDeviceInventory(BareosSocket* sd,
                            const std::vector<StorageResource*>& stores);
} /* namespace directordaemon */
#endif  // BAREOS_DIRD_STORAGE_H_
