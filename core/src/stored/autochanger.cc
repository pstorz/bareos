/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
   Copyright (C) 2013-2026 Bareos GmbH & Co. KG

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
// Kern Sibbald, August MMII
/**
 * @file
 * Routines for handling the autochanger.
 */

#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/autochanger.h"
#include "stored/device_control_record.h"
#include "stored/wait.h"
#include "lib/berrno.h"
#include "lib/bnet.h"
#include "lib/edit.h"
#include "lib/util.h"
#include "lib/parse_conf.h"
#include "lib/bsock.h"
#include "lib/berrno.h"
#include "lib/bpipe.h"
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
#  include "lib/scsi_changer.h"
#  include <fcntl.h>
#endif

namespace storagedaemon {

/* Forward referenced functions */
static bool LockChanger(DeviceControlRecord* dcr);
static bool UnlockChanger(DeviceControlRecord* dcr);
static bool UnloadOtherDrive(DeviceControlRecord* dcr,
                             slot_number_t slot,
                             bool lock_set);
static char* transfer_edit_device_codes(DeviceControlRecord* dcr,
                                        POOLMEM*& omsg,
                                        const char* imsg,
                                        const char* cmd,
                                        slot_number_t src_slot,
                                        slot_number_t dst_slot);

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
/* ── Native SCSI helpers ─────────────────────────────────────────────────── */

/** Return true if this changer should use native SCSI instead of a script. */
static bool ChangerUsesNativeScsi(DeviceControlRecord* dcr)
{
  AutochangerResource* changer_res = dcr->device_resource->changer_res;
  return changer_res && changer_res->use_native_scsi;
}

/**
 * Open the SCSI generic device for the changer and return its fd.
 * The caller is responsible for closing it with close(fd).
 * Returns -1 on failure (error already logged).
 */
static int OpenChangerFd(DeviceControlRecord* dcr)
{
  const char* changer_name = dcr->device_resource->changer_name;
  if (!changer_name) {
    Jmsg(dcr->jcr, M_ERROR, 0, T_("No Changer Device name configured for %s\n"),
         dcr->dev->print_name());
    return -1;
  }

  int fd = open(changer_name, O_RDWR | O_NONBLOCK | O_BINARY);
  if (fd < 0) {
    BErrNo be;
    Jmsg(dcr->jcr, M_ERROR, 0, T_("Cannot open changer device %s: ERR=%s\n"),
         changer_name, be.bstrerror());
  }
  return fd;
}

/**
 * Translate logical slot number (1-based) to SCSI element address.
 * Returns 0 if the slot is not found in the inventory.
 */
static uint16_t SlotToScsiAddr(slot_number_t logical_slot,
                               const bareos::scsi::ChangerInventory& inv)
{
  for (const auto& s : inv.slots) {
    if (s.logical_slot == logical_slot) { return s.address; }
  }
  for (const auto& s : inv.ie_slots) {
    if (s.logical_slot == logical_slot) { return s.address; }
  }
  return 0;
}

/** Native SCSI implementation of the "loaded" query for a single drive. */
static slot_number_t GetAutochangerLoadedSlotNative(DeviceControlRecord* dcr)
{
  int fd = OpenChangerFd(dcr);
  if (fd < 0) { return kInvalidSlotNumber; }

  bareos::scsi::ChangerInventory inv;
  bool ok = bareos::scsi::ReadElementStatus(
      fd, dcr->device_resource->changer_name, inv);
  close(fd);
  if (!ok) { return kInvalidSlotNumber; }

  drive_number_t want_drive = dcr->dev->drive_index;
  for (const auto& d : inv.drives) {
    if (d.logical_drive == want_drive) {
      if (d.loaded) {
        dcr->dev->SetSlotNumber(d.loaded_from_slot);
        return d.loaded_from_slot;
      } else {
        dcr->dev->SetSlotNumber(0);
        return 0;
      }
    }
  }
  return kInvalidSlotNumber;
}

/** Native SCSI implementation of the "load" command. */
static int AutoloadDeviceNative(DeviceControlRecord* dcr,
                                int writing,
                                BareosSocket* dir)
{
  JobControlRecord* jcr = dcr->jcr;
  drive_number_t drive = dcr->dev->drive_index;
  slot_number_t wanted_slot
      = dcr->VolCatInfo.InChanger ? dcr->VolCatInfo.Slot : 0;

  if (!IsSlotNumberValid(wanted_slot)) {
    if (writing && dir) { return 0; }
    if (!dcr->DirFindNextAppendableVolume()) { return 0; }
    wanted_slot = dcr->VolCatInfo.InChanger ? dcr->VolCatInfo.Slot : 0;
    if (!IsSlotNumberValid(wanted_slot)) { return 0; }
  }

  int fd = OpenChangerFd(dcr);
  if (fd < 0) { return -1; }

  bareos::scsi::ChangerInventory inv;
  if (!bareos::scsi::ReadElementStatus(fd, dcr->device_resource->changer_name,
                                       inv)) {
    close(fd);
    return -1;
  }

  /* Find the drive's current state */
  uint16_t drive_addr = 0;
  slot_number_t loaded_slot = 0;
  for (const auto& d : inv.drives) {
    if (d.logical_drive == drive) {
      drive_addr = d.address;
      loaded_slot = d.loaded ? d.loaded_from_slot : 0;
      break;
    }
  }
  if (!drive_addr) {
    Jmsg(jcr, M_ERROR, 0, T_("Drive %hd not found in changer inventory\n"),
         drive);
    close(fd);
    return -1;
  }

  /* Already loaded — nothing to do */
  if (loaded_slot == wanted_slot && IsSlotNumberValid(loaded_slot)) {
    dcr->dev->SetSlotNumber(wanted_slot);
    close(fd);
    return 1;
  }

  if (!LockChanger(dcr)) {
    close(fd);
    return -2;
  }

  /* Unload whatever is in the drive now */
  if (IsSlotNumberValid(loaded_slot)) {
    uint16_t ret_slot_addr = SlotToScsiAddr(loaded_slot, inv);
    if (!ret_slot_addr) {
      ret_slot_addr
          = static_cast<uint16_t>(inv.first_slot_address + loaded_slot - 1);
    }
    Jmsg(jcr, M_INFO, 0,
         T_("3307 Issuing autochanger \"unload slot %hd, drive %hd\" "
            "command.\n"),
         loaded_slot, drive);
    dcr->dev->close(dcr);
    if (!bareos::scsi::MoveMedium(fd, dcr->device_resource->changer_name,
                                  drive_addr, ret_slot_addr)) {
      Jmsg(jcr, M_ERROR, 0,
           T_("3995 Autochanger unload slot %hd, drive %hd failed\n"),
           loaded_slot, drive);
      UnlockChanger(dcr);
      close(fd);
      return -1;
    }
    dcr->dev->SetSlotNumber(0);
  }

  /* Unload the wanted slot from any other drive that might have it */
  uint16_t wanted_slot_addr = SlotToScsiAddr(wanted_slot, inv);
  if (!wanted_slot_addr) {
    wanted_slot_addr
        = static_cast<uint16_t>(inv.first_slot_address + wanted_slot - 1);
  }
  for (const auto& d : inv.drives) {
    if (d.logical_drive != drive && d.loaded
        && d.loaded_from_slot == wanted_slot) {
      Jmsg(jcr, M_INFO, 0, T_("3307 Unloading slot %hd from drive %hd\n"),
           wanted_slot, d.logical_drive);
      if (!bareos::scsi::MoveMedium(fd, dcr->device_resource->changer_name,
                                    d.address, wanted_slot_addr)) {
        UnlockChanger(dcr);
        close(fd);
        return -1;
      }
    }
  }

  /* Load the wanted slot into the drive */
  Jmsg(jcr, M_INFO, 0,
       T_("3304 Issuing autochanger \"load slot %hd, drive %hd\" "
          "command.\n"),
       wanted_slot, drive);
  dcr->dev->close(dcr);
  if (!bareos::scsi::MoveMedium(fd, dcr->device_resource->changer_name,
                                wanted_slot_addr, drive_addr)) {
    int rtn_stat = -1;
    if (errno == ENOMEDIUM) {
      Jmsg(jcr, M_ERROR, 0,
           T_("3992 Autochanger load slot %hd: slot is empty\n"), wanted_slot);
      rtn_stat = -3;
    } else {
      Jmsg(jcr, M_FATAL, 0,
           T_("3992 Autochanger load slot %hd, drive %hd failed\n"),
           wanted_slot, drive);
    }
    UnlockChanger(dcr);
    close(fd);
    return rtn_stat;
  }

  Jmsg(jcr, M_INFO, 0,
       T_("3305 Autochanger \"load slot %hd, drive %hd\", status is OK.\n"),
       wanted_slot, drive);
  dcr->dev->SetSlotNumber(wanted_slot);
  if (dcr->dev->vol) { dcr->dev->vol->ClearSwapping(); }

  UnlockChanger(dcr);
  close(fd);
  return 1;
}

/** Native SCSI implementation of the "unload" command. */
static bool UnloadAutochangerNative(DeviceControlRecord* dcr,
                                    slot_number_t loaded_slot,
                                    bool lock_set)
{
  Device* dev = dcr->dev;
  JobControlRecord* jcr = dcr->jcr;
  bool retval = true;

  if (loaded_slot == 0) { return true; }

  if (!lock_set) {
    if (!LockChanger(dcr)) { return false; }
  }

  if (loaded_slot == kInvalidSlotNumber) {
    loaded_slot = GetAutochangerLoadedSlotNative(dcr);
  }

  if (!IsSlotNumberValid(loaded_slot)) {
    if (!lock_set) { UnlockChanger(dcr); }
    return true; /* nothing to unload */
  }

  int fd = OpenChangerFd(dcr);
  if (fd < 0) {
    if (!lock_set) { UnlockChanger(dcr); }
    return false;
  }

  bareos::scsi::ChangerInventory inv;
  if (!bareos::scsi::ReadElementStatus(fd, dcr->device_resource->changer_name,
                                       inv)) {
    close(fd);
    if (!lock_set) { UnlockChanger(dcr); }
    return false;
  }

  drive_number_t drive = dev->drive_index;
  uint16_t drive_addr = 0;
  for (const auto& d : inv.drives) {
    if (d.logical_drive == drive) {
      drive_addr = d.address;
      break;
    }
  }

  uint16_t slot_addr = SlotToScsiAddr(loaded_slot, inv);
  if (!slot_addr) {
    slot_addr = static_cast<uint16_t>(inv.first_slot_address + loaded_slot - 1);
  }

  Jmsg(jcr, M_INFO, 0,
       T_("3307 Issuing autochanger \"unload slot %hd, drive %hd\" "
          "command.\n"),
       loaded_slot, dev->drive);
  dev->close(dcr);

  if (!bareos::scsi::MoveMedium(fd, dcr->device_resource->changer_name,
                                drive_addr, slot_addr)) {
    BErrNo be;
    Jmsg(jcr, M_INFO, 0,
         T_("3995 Bad autochanger \"unload slot %hd, drive %hd\": ERR=%s\n"),
         loaded_slot, dev->drive, be.bstrerror());
    retval = false;
    dev->InvalidateSlotNumber();
  } else {
    dev->SetSlotNumber(0);
  }

  close(fd);
  if (!lock_set) { UnlockChanger(dcr); }

  if (IsSlotNumberValid(loaded_slot)) { FreeVolume(dev); }
  if (retval) { dev->ClearUnload(); }

  return retval;
}

/** Native SCSI implementation of list/listall/slots commands. */
static bool AutochangerCmdNative(DeviceControlRecord* dcr,
                                 BareosSocket* dir,
                                 const char* cmd)
{
  int fd = OpenChangerFd(dcr);
  if (fd < 0) {
    dir->fsend(T_("3997 Cannot open changer device.\n"));
    return false;
  }

  bareos::scsi::ChangerInventory inv;
  if (!bareos::scsi::ReadElementStatus(fd, dcr->device_resource->changer_name,
                                       inv)) {
    close(fd);
    dir->fsend(T_("3997 READ ELEMENT STATUS failed.\n"));
    return false;
  }
  close(fd);

  if (bstrcmp(cmd, "slots")) {
    dir->fsend("slots=%hd", inv.num_slots);
    return true;
  }

  if (bstrcmp(cmd, "list")) {
    for (const auto& s : inv.slots) {
      if (s.full) { dir->fsend("%hd:%s\n", s.logical_slot, s.barcode); }
    }
    return true;
  }

  if (bstrcmp(cmd, "listall")) {
    for (const auto& d : inv.drives) {
      if (d.loaded) {
        dir->fsend("D:%hd:F:%hd:%s\n", d.logical_drive, d.loaded_from_slot,
                   d.barcode);
      } else {
        dir->fsend("D:%hd:E\n", d.logical_drive);
      }
    }
    for (const auto& s : inv.slots) {
      if (s.full) {
        dir->fsend("S:%hd:F:%s\n", s.logical_slot, s.barcode);
      } else {
        dir->fsend("S:%hd:E\n", s.logical_slot);
      }
    }
    for (const auto& ie : inv.ie_slots) {
      if (ie.full) {
        dir->fsend("I:%hd:F:%s\n", ie.logical_slot, ie.barcode);
      } else {
        dir->fsend("I:%hd:E\n", ie.logical_slot);
      }
    }
    return true;
  }

  return false;
}

/** Native SCSI implementation of the "transfer" command. */
static bool AutochangerTransferCmdNative(DeviceControlRecord* dcr,
                                         BareosSocket* dir,
                                         slot_number_t src_slot,
                                         slot_number_t dst_slot)
{
  int fd = OpenChangerFd(dcr);
  if (fd < 0) {
    dir->fsend(T_("3997 Cannot open changer device.\n"));
    return false;
  }

  bareos::scsi::ChangerInventory inv;
  if (!bareos::scsi::ReadElementStatus(fd, dcr->device_resource->changer_name,
                                       inv)) {
    close(fd);
    dir->fsend(T_("3997 READ ELEMENT STATUS failed.\n"));
    return false;
  }

  uint16_t src_addr = SlotToScsiAddr(src_slot, inv);
  uint16_t dst_addr = SlotToScsiAddr(dst_slot, inv);

  if (!src_addr) {
    src_addr = static_cast<uint16_t>(inv.first_slot_address + src_slot - 1);
  }
  if (!dst_addr) {
    dst_addr = static_cast<uint16_t>(inv.first_slot_address + dst_slot - 1);
  }

  bool ok = bareos::scsi::MoveMedium(fd, dcr->device_resource->changer_name,
                                     src_addr, dst_addr);
  close(fd);

  if (ok) {
    dir->fsend(
        T_("3308 Successfully transferred volume from slot %hd to %hd.\n"),
        src_slot, dst_slot);
  } else {
    dir->fsend(T_("3998 Autochanger transfer %hd→%hd failed.\n"), src_slot,
               dst_slot);
  }

  return ok;
}
#endif /* HAVE_LOWLEVEL_SCSI_INTERFACE */

// Init all the autochanger resources found
bool InitAutochangers()
{
  bool OK = true;
  AutochangerResource* changer;
  drive_number_t logical_drive_number;

  // Ensure that the media_type for each device is the same
  foreach_res (changer, R_AUTOCHANGER) {
    logical_drive_number = 0;
    for (auto* device_resource : changer->device_resources) {
      /* If the device does not have a changer name or changer command
       * defined, used the one from the Autochanger resource */
      if (!device_resource->changer_name && changer->changer_name) {
        device_resource->changer_name = strdup(changer->changer_name);
      }

      if (!device_resource->changer_command && changer->changer_command) {
        device_resource->changer_command = strdup(changer->changer_command);
      }

      if (!device_resource->changer_name) {
        Jmsg(NULL, M_ERROR, 0,
             T_("No Changer Name given for device %s. Cannot continue.\n"),
             device_resource->resource_name_);
        OK = false;
      }

      if (!device_resource->changer_command) {
        if (!changer->use_native_scsi) {
          Jmsg(NULL, M_ERROR, 0,
               T_("No Changer Command given for device %s. Cannot "
                  "continue.\n"),
               device_resource->resource_name_);
          OK = false;
        }
      }

      // Give the drive in the autochanger a logical drive number.
      device_resource->drive = logical_drive_number++;
    }
  }

  return OK;
}

/**
 * Called here to do an autoload using the autochanger, if
 * configured, and if a Slot has been defined for this Volume.
 * On success this routine loads the indicated tape, but the
 * label is not read, so it must be verified.
 *
 * Note if dir is not NULL, it is the console requesting the
 * autoload for labeling, so we respond directly to the dir bsock.
 *
 * Returns: 1 on success
 *          0 on failure (no changer available)
 *         -1 on error on autochanger
 *         -2 on error locking the autochanger
 */
int AutoloadDevice(DeviceControlRecord* dcr, int writing, BareosSocket* dir)
{
  POOLMEM* changer;
  int rtn_stat = -1; /* error status */
  slot_number_t wanted_slot;
  JobControlRecord* jcr = dcr->jcr;
  drive_number_t drive;

  if (!dcr->dev->AttachedToAutochanger()) {
    Dmsg1(100, "Device %s is not attached to an autochanger\n",
          dcr->dev->print_name());
    return 0;
  }

  // An empty ChangerCommand => virtual disk autochanger
  if (dcr->device_resource->changer_command
      && dcr->device_resource->changer_command[0] == 0) {
    Dmsg0(100, "ChangerCommand=0, virtual disk changer\n");
    return 1; /* nothing to load */
  }

  drive = dcr->dev->drive_index;
  wanted_slot = dcr->VolCatInfo.InChanger ? dcr->VolCatInfo.Slot : 0;
  Dmsg3(100, "autoload: slot=%" PRId32 " InChgr=%d Vol=%s\n",
        dcr->VolCatInfo.Slot, dcr->VolCatInfo.InChanger, dcr->getVolCatName());

  /* Handle autoloaders here.  If we cannot autoload it, we will return 0 so
   * that the sysop will be asked to load it. */
  if (writing && (!IsSlotNumberValid(wanted_slot))) {
    if (dir) { return 0; /* For user, bail out right now */ }

    // this really should not be here
    if (dcr->DirFindNextAppendableVolume()) {
      wanted_slot = dcr->VolCatInfo.InChanger ? dcr->VolCatInfo.Slot : 0;
    } else {
      wanted_slot = 0;
    }
  }
  Dmsg1(400, "Want changer slot=%hd\n", wanted_slot);

  changer = GetPoolMemory(PM_FNAME);
  if (!IsSlotNumberValid(wanted_slot)) {
    // Suppress info when polling
    if (!dcr->dev->poll) {
      Jmsg(jcr, M_INFO, 0,
           T_("No slot defined in catalog (slot=%hd) for Volume \"%s\" on "
              "%s.\n"),
           wanted_slot, dcr->getVolCatName(), dcr->dev->print_name());
      Jmsg(jcr, M_INFO, 0,
           T_("Cartridge change or \"update slots\" may be required.\n"));
    }
    rtn_stat = 0;
  } else if (!dcr->device_resource->changer_name) {
    // Suppress info when polling
    if (!dcr->dev->poll) {
      Jmsg(jcr, M_INFO, 0,
           T_("No \"Changer Device\" for %s. Manual load of Volume may be "
              "required.\n"),
           dcr->dev->print_name());
    }
    rtn_stat = 0;
  } else if (!dcr->device_resource->changer_command
             && !dcr->device_resource->changer_res->use_native_scsi) {
    // Suppress info when polling
    if (!dcr->dev->poll) {
      Jmsg(jcr, M_INFO, 0,
           T_("No \"Changer Command\" for %s. Manual load of Volume may be "
              "required.\n"),
           dcr->dev->print_name());
    }
    rtn_stat = 0;
  } else {
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
    if (ChangerUsesNativeScsi(dcr)) {
      FreePoolMemory(changer);
      return AutoloadDeviceNative(dcr, writing, dir);
    }
#endif
    uint32_t timeout = dcr->device_resource->max_changer_wait;
    int status;
    slot_number_t loaded_slot;

    // Attempt to load the Volume
    loaded_slot = GetAutochangerLoadedSlot(dcr);
    if (loaded_slot != wanted_slot) {
      PoolMem results(PM_MESSAGE);

      if (!LockChanger(dcr)) {
        rtn_stat = -2;
        goto bail_out;
      }

      // Unload anything in our drive
      if (!UnloadAutochanger(dcr, loaded_slot, true)) {
        UnlockChanger(dcr);
        goto bail_out;
      }

      // Make sure desired slot is unloaded
      if (!UnloadOtherDrive(dcr, wanted_slot, true)) {
        UnlockChanger(dcr);
        goto bail_out;
      }

      // Load the desired volume.
      Dmsg2(100, "Doing changer load slot %hd %s\n", wanted_slot,
            dcr->dev->print_name());
      Jmsg(jcr, M_INFO, 0,
           T_("3304 Issuing autochanger \"load slot %hd, drive %hd\" "
              "command.\n"),
           wanted_slot, drive);
      dcr->VolCatInfo.Slot = wanted_slot; /* slot to be loaded */
      changer = edit_device_codes(
          dcr, changer, dcr->device_resource->changer_command, "load");
      dcr->dev->close(dcr);
      Dmsg1(200, "Run program=%s\n", changer);
      status = RunProgramFullOutput(changer, timeout, results.addr());
      if (status == 0) {
        Jmsg(jcr, M_INFO, 0,
             T_("3305 Autochanger \"load slot %hd, drive %hd\", status is "
                "OK.\n"),
             wanted_slot, drive);
        Dmsg2(100, "load slot %hd, drive %hd, status is OK.\n", wanted_slot,
              drive);
        dcr->dev->SetSlotNumber(wanted_slot); /* set currently loaded slot */
        if (dcr->dev->vol) {
          // We just swapped this Volume so it cannot be swapping any more
          dcr->dev->vol->ClearSwapping();
        }
      } else {
        BErrNo be;
        be.SetErrno(status);
        std::string tmp(results.c_str());
        if (tmp.find("Source Element Address") != std::string::npos
            && tmp.find("is Empty") != std::string::npos) {
          rtn_stat = -3; /* medium not found in slot */
        } else {
          rtn_stat = -1; /* hard error */
        }
        Dmsg3(100, "load slot %hd, drive %hd, bad stats=%s.\n", wanted_slot,
              drive, be.bstrerror());
        Jmsg(jcr, rtn_stat == -3 ? M_ERROR : M_FATAL, 0,
             T_("3992 Bad autochanger \"load slot %hd, drive %hd\": "
                "ERR=%s.\nResults=%s\n"),
             wanted_slot, drive, be.bstrerror(), results.c_str());
        dcr->dev->SetSlotNumber(-1); /* mark unknown */
      }
      Dmsg2(100, "load slot %hd status=%d\n", wanted_slot, status);
      UnlockChanger(dcr);
    } else {
      status = 0;                           /* we got what we want */
      dcr->dev->SetSlotNumber(wanted_slot); /* set currently loaded slot */
    }

    Dmsg1(100, "After changer, status=%d\n", status);

    if (status == 0) { /* did we succeed? */
      rtn_stat = 1;    /* tape loaded by changer */
    }
  }

bail_out:
  FreePoolMemory(changer);
  return rtn_stat;
}

/**
 * Returns: -1 if error from changer command
 *          slot otherwise
 *
 * Note, this is safe to do without releasing the drive since it does not
 * attempt load/unload a slot.
 */
slot_number_t GetAutochangerLoadedSlot(DeviceControlRecord* dcr, bool lock_set)
{
  int status;
  POOLMEM* changer;
  JobControlRecord* jcr = dcr->jcr;
  slot_number_t loaded_slot = kInvalidSlotNumber;
  Device* dev = dcr->dev;
  PoolMem results(PM_MESSAGE);
  uint32_t timeout = dcr->device_resource->max_changer_wait;
  drive_number_t drive = dcr->dev->drive;

  if (!dev->AttachedToAutochanger()) { return kInvalidSlotNumber; }

  if (!dcr->device_resource->changer_command
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
      && !ChangerUsesNativeScsi(dcr)
#endif
  ) {
    return kInvalidSlotNumber;
  }

  slot_number_t slot = dev->GetSlot();
  if (IsSlotNumberValid(slot)) { return slot; }

  // Virtual disk autochanger (only when using script path)
  if (dcr->device_resource->changer_command
      && dcr->device_resource->changer_command[0] == 0) {
    return 1;
  }

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  if (ChangerUsesNativeScsi(dcr)) {
    if (!lock_set) {
      if (!LockChanger(dcr)) { return kInvalidSlotNumber; }
    }
    slot_number_t result = GetAutochangerLoadedSlotNative(dcr);
    if (!lock_set) { UnlockChanger(dcr); }
    return result;
  }
#endif

  /* Only lock the changer if the lock_set is false e.g. changer not locked by
   * calling function. */
  if (!lock_set) {
    if (!LockChanger(dcr)) { return kInvalidSlotNumber; }
  }

  /* Find out what is loaded, zero means device is unloaded
   * Suppress info when polling */
  if (!dev->poll && debug_level >= 1) {
    Jmsg(jcr, M_INFO, 0,
         T_("3301 Issuing autochanger \"loaded? drive %hd\" command.\n"),
         drive);
  }

  changer = GetPoolMemory(PM_FNAME);
  changer = edit_device_codes(dcr, changer,
                              dcr->device_resource->changer_command, "loaded");
  Dmsg1(100, "Run program=%s\n", changer);
  status = RunProgramFullOutput(changer, timeout, results.addr());
  Dmsg3(100, "run_prog: %s stat=%d result=%s\n", changer, status,
        results.c_str());

  if (status == 0) {
    loaded_slot = str_to_uint16(results.c_str());
    if (IsSlotNumberValid(loaded_slot)) {
      // Suppress info when polling
      if (!dev->poll && debug_level >= 1) {
        Jmsg(
            jcr, M_INFO, 0,
            T_("3302 Autochanger \"loaded? drive %hd\", result is Slot %hd.\n"),
            drive, loaded_slot);
      }
      dev->SetSlotNumber(loaded_slot);
    } else {
      // Suppress info when polling
      if (!dev->poll && debug_level >= 1) {
        Jmsg(jcr, M_INFO, 0,
             T_("3302 Autochanger \"loaded? drive %hd\", result: nothing "
                "loaded.\n"),
             drive);
      }
      dev->SetSlotNumber(0);
    }
  } else {
    BErrNo be;
    be.SetErrno(status);
    Jmsg(jcr, M_INFO, 0,
         T_("3991 Bad autochanger \"loaded? drive %hd\" command: "
            "ERR=%s.\nResults=%s\n"),
         drive, be.bstrerror(), results.c_str());
    loaded_slot = kInvalidSlotNumber; /* force unload */
  }

  if (!lock_set) { UnlockChanger(dcr); }

  FreePoolMemory(changer);

  return loaded_slot;
}

static bool LockChanger(DeviceControlRecord* dcr)
{
  AutochangerResource* changer_res = dcr->device_resource->changer_res;

  if (changer_res) {
    int errstat;
    Dmsg1(200, "Locking changer %s\n", changer_res->resource_name_);
    if ((errstat = RwlWritelock(&changer_res->changer_lock)) != 0) {
      BErrNo be;
      Jmsg(dcr->jcr, M_ERROR_TERM, 0,
           T_("Lock failure on autochanger. ERR=%s\n"), be.bstrerror(errstat));
    }

    /* We just locked the changer for exclusive use so let any plugin know we
     * have. */
    if (GeneratePluginEvent(dcr->jcr, bSdEventChangerLock, dcr) != bRC_OK) {
      Dmsg0(100, "Locking changer: bSdEventChangerLock failed\n");
      RwlWriteunlock(&changer_res->changer_lock);
      return false;
    }
  }

  return true;
}

static bool UnlockChanger(DeviceControlRecord* dcr)
{
  AutochangerResource* changer_res = dcr->device_resource->changer_res;

  if (changer_res) {
    int errstat;

    GeneratePluginEvent(dcr->jcr, bSdEventChangerUnlock, dcr);

    Dmsg1(200, "Unlocking changer %s\n", changer_res->resource_name_);
    if ((errstat = RwlWriteunlock(&changer_res->changer_lock)) != 0) {
      BErrNo be;
      Jmsg(dcr->jcr, M_ERROR_TERM, 0,
           T_("Unlock failure on autochanger. ERR=%s\n"),
           be.bstrerror(errstat));
    }
  }

  return true;
}

/**
 * Unload the volume, if any, in this drive
 * On entry: loaded_slot == 0                   -- nothing to do
 *           loaded_slot  == kInvalidSlotNumber -- check if anything to do
 *           loaded_slot  > 0                   -- load slot == loaded_slot
 */
bool UnloadAutochanger(DeviceControlRecord* dcr,
                       slot_number_t loaded_slot,
                       bool lock_set)
{
  Device* dev = dcr->dev;
  JobControlRecord* jcr = dcr->jcr;
  slot_number_t slot;
  uint32_t timeout = dcr->device_resource->max_changer_wait;
  bool retval = true;

  if (loaded_slot == 0) { return true; }

  if (!dev->AttachedToAutochanger() || !dcr->device_resource->changer_name) {
    return false;
  }

  bool have_script = dcr->device_resource->changer_command != nullptr;
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  bool have_native = ChangerUsesNativeScsi(dcr);
#else
  bool have_native = false;
#endif
  if (!have_script && !have_native) { return false; }

  // Virtual disk autochanger (script path, empty command string)
  if (have_script && dcr->device_resource->changer_command[0] == 0) {
    dev->ClearUnload();
    return true;
  }

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  if (have_native) {
    return UnloadAutochangerNative(dcr, loaded_slot, lock_set);
  }
#endif

  /* Only lock the changer if the lock_set is false e.g. changer not locked by
   * calling function. */
  if (!lock_set) {
    if (!LockChanger(dcr)) { return false; }
  }

  if (loaded_slot == kInvalidSlotNumber) {
    loaded_slot = GetAutochangerLoadedSlot(dcr, true);
  }

  if (IsSlotNumberValid(loaded_slot)) {
    int status;
    PoolMem results(PM_MESSAGE);
    POOLMEM* changer = GetPoolMemory(PM_FNAME);

    Jmsg(jcr, M_INFO, 0,
         T_("3307 Issuing autochanger \"unload slot %hd, drive %hd\" "
            "command.\n"),
         loaded_slot, dev->drive);
    slot = dcr->VolCatInfo.Slot;
    dcr->VolCatInfo.Slot = loaded_slot;
    changer = edit_device_codes(
        dcr, changer, dcr->device_resource->changer_command, "unload");
    dev->close(dcr);
    Dmsg1(100, "Run program=%s\n", changer);
    status = RunProgramFullOutput(changer, timeout, results.addr());
    dcr->VolCatInfo.Slot = slot;
    if (status != 0) {
      BErrNo be;

      be.SetErrno(status);
      Jmsg(jcr, M_INFO, 0,
           T_("3995 Bad autochanger \"unload slot %hd, drive %hd\": "
              "ERR=%s\nResults=%s\n"),
           loaded_slot, dev->drive, be.bstrerror(), results.c_str());
      retval = false;
      dev->InvalidateSlotNumber(); /* unknown */
    } else {
      dev->SetSlotNumber(0); /* nothing loaded */
    }

    FreePoolMemory(changer);
  }

  /* Only unlock the changer if the lock_set is false e.g. changer not locked by
   * calling function. */
  if (!lock_set) { UnlockChanger(dcr); }

  // FreeVolume outside from changer lock
  if (IsSlotNumberValid(loaded_slot)) {
    FreeVolume(dev); /* Free any volume associated with this drive */
  }

  if (retval) { dev->ClearUnload(); }

  return retval;
}

// Unload the slot if mounted in a different drive
static bool UnloadOtherDrive(DeviceControlRecord* dcr,
                             slot_number_t slot,
                             bool lock_set)
{
  Device* dev = NULL;
  Device* dev_save;
  bool found = false;
  AutochangerResource* changer = dcr->dev->device_resource->changer_res;
  int retries = 0; /* wait for device retries */

  if (!changer) { return false; }
  if (changer->device_resources->size() == 1) { return true; }

  /* We look for the slot number corresponding to the tape
   * we want in other drives, and if possible, unload it. */
  Dmsg0(100, "Wiffle through devices looking for slot\n");
  for (auto* device_resource : changer->device_resources) {
    dev = device_resource->dev;
    if (!dev) { continue; }
    dev_save = dcr->dev;
    dcr->SetDev(dev);

    bool slotnumber_not_set = !IsSlotNumberValid(dev->GetSlot());
    bool slot_not_loaded_in_autochanger
        = !IsSlotNumberValid(GetAutochangerLoadedSlot(dcr, lock_set));

    if (slotnumber_not_set && slot_not_loaded_in_autochanger) {
      dcr->SetDev(dev_save);
      continue;
    }

    dcr->SetDev(dev_save);
    if (dev->GetSlot() == slot) {
      found = true;
      break;
    }
  }
  if (!found) {
    Dmsg1(100, "Slot=%hd not found in another device\n", slot);
    return true;
  } else {
    Dmsg1(100, "Slot=%hd found in another device\n", slot);
  }

  // The Volume we want is on another device.
  if (dev->IsBusy() || dev->IsBlocked()) {
    Dmsg4(100, "Vol %s for dev=%s in use dev=%s slot=%hd\n", dcr->VolumeName,
          dcr->dev->print_name(), dev->print_name(), slot);
  }

  for (int i = 0; i < 3; i++) {
    if (dev->IsBusy() || dev->IsBlocked()) {
      WaitForDevice(dcr->jcr, retries);
      continue;
    }
    break;
  }

  if (dev->IsBusy() || dev->IsBlocked()) {
    Jmsg(dcr->jcr, M_WARNING, 0,
         T_("Volume \"%s\" wanted on %s is in use by device %s\n"),
         dcr->VolumeName, dcr->dev->print_name(), dev->print_name());
    Dmsg4(100, "Vol %s for dev=%s is busy dev=%s slot=%hd\n", dcr->VolumeName,
          dcr->dev->print_name(), dev->print_name(), dev->GetSlot());
    Dmsg2(100, "num_writ=%d reserved=%d\n", dev->num_writers,
          dev->NumReserved());
    VolumeUnused(dcr);

    return false;
  }

  return UnloadDev(dcr, dev, lock_set);
}

// Unconditionally unload a specified drive
bool UnloadDev(DeviceControlRecord* dcr, Device* dev, bool lock_set)
{
  int status;
  Device* save_dev;
  bool retval = true;
  JobControlRecord* jcr = dcr->jcr;
  slot_number_t save_slot;
  uint32_t timeout = dcr->device_resource->max_changer_wait;
  AutochangerResource* changer = dcr->dev->device_resource->changer_res;

  if (!changer) { return false; }

  save_dev = dcr->dev; /* save dcr device */
  dcr->SetDev(dev);    /* temporarily point dcr at other device */

  // Update slot if not set or not always_open
  slot_number_t slot = dev->GetSlot();
  if (!IsSlotNumberValid(slot) || !dev->HasCap(CAP_ALWAYSOPEN)) {
    GetAutochangerLoadedSlot(dcr, lock_set);
  }

  // Fail if we have no slot to unload
  slot = dev->GetSlot();
  if (!IsSlotNumberValid(slot)) {
    dcr->SetDev(save_dev);
    return false;
  }

  /* Only lock the changer if the lock_set is false e.g. changer not locked by
   * calling function. */
  if (!lock_set) {
    if (!LockChanger(dcr)) {
      dcr->SetDev(save_dev);
      return false;
    }
  }

  save_slot = dcr->VolCatInfo.Slot;
  dcr->VolCatInfo.Slot = dev->GetSlot();

  POOLMEM* ChangerCmd = GetPoolMemory(PM_FNAME);
  PoolMem results(PM_MESSAGE);

  Jmsg(jcr, M_INFO, 0,
       T_("3307 Issuing autochanger \"unload slot %hd, drive %hd\" command.\n"),
       dev->GetSlot(), dev->drive);

  Dmsg2(100, "Issuing autochanger \"unload slot %hd, drive %hd\" command.\n",
        dev->GetSlot(), dev->drive);

  ChangerCmd = edit_device_codes(
      dcr, ChangerCmd, dcr->device_resource->changer_command, "unload");
  dev->close(dcr);

  Dmsg2(200, "close dev=%s reserve=%d\n", dev->print_name(),
        dev->NumReserved());
  Dmsg1(100, "Run program=%s\n", ChangerCmd);

  status = RunProgramFullOutput(ChangerCmd, timeout, results.addr());
  dcr->VolCatInfo.Slot = save_slot;
  dcr->SetDev(save_dev);
  if (status != 0) {
    BErrNo be;
    be.SetErrno(status);
    Jmsg(jcr, M_INFO, 0,
         T_("3997 Bad autochanger \"unload slot %hd, drive %hd\": ERR=%s.\n"),
         dev->GetSlot(), dev->drive, be.bstrerror());

    Dmsg3(100, "Bad autochanger \"unload slot %hd, drive %hd\": ERR=%s.\n",
          dev->GetSlot(), dev->drive, be.bstrerror());
    retval = false;
    dev->InvalidateSlotNumber(); /* unknown */
  } else {
    Dmsg2(100, "Slot %hd unloaded %s\n", dev->GetSlot(), dev->print_name());
    dev->SetSlotNumber(0); /* nothing loaded */
  }
  if (retval) { dev->ClearUnload(); }

  /* Only unlock the changer if the lock_set is false e.g. changer not locked by
   * calling function. */
  if (!lock_set) { UnlockChanger(dcr); }

  FreeVolume(dev); /* Free any volume associated with this drive */
  FreePoolMemory(ChangerCmd);

  return retval;
}

/**
 * List the Volumes that are in the autoloader possibly with their barcodes.
 * We assume that it is always the Console that is calling us.
 */
bool AutochangerCmd(DeviceControlRecord* dcr,
                    BareosSocket* dir,
                    const char* cmd)
{
  Device* dev = dcr->dev;
  uint32_t timeout = dcr->device_resource->max_changer_wait;
  POOLMEM* changer;
  Bpipe* bpipe;
  int len = SizeofPoolMemory(dir->msg) - 1;
  int status;
  int retries = 1; /* Number of retries on failing slot count */

  if (!dev->AttachedToAutochanger() || !dcr->device_resource->changer_name) {
    if (bstrcmp(cmd, "drives")) { dir->fsend("drives=1\n"); }
    dir->fsend(T_("3993 Device %s not an autochanger device.\n"),
               dev->print_name());
    return false;
  }

  {
    bool have_script = dcr->device_resource->changer_command != nullptr;
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
    bool have_native = ChangerUsesNativeScsi(dcr);
#else
    bool have_native = false;
#endif
    if (!have_script && !have_native && !bstrcmp(cmd, "drives")) {
      dir->fsend(T_("3993 Device %s not an autochanger device.\n"),
                 dev->print_name());
      return false;
    }
  }

  if (bstrcmp(cmd, "drives")) {
    AutochangerResource* changer_res = dcr->device_resource->changer_res;
    int drives = 1;
    if (changer_res) { drives = changer_res->device_resources->size(); }
    dir->fsend("drives=%d\n", drives);
    Dmsg1(100, "drives=%d\n", drives);
    return true;
  }

  // If listing, reprobe changer
  if (bstrcmp(cmd, "list") || bstrcmp(cmd, "listall")) {
    dcr->dev->SetSlotNumber(0);
    GetAutochangerLoadedSlot(dcr);
  }

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  if (ChangerUsesNativeScsi(dcr)) {
    LockChanger(dcr);
    dir->fsend(T_("3306 Issuing autochanger \"%s\" command.\n"), cmd);
    bool ok = AutochangerCmdNative(dcr, dir, cmd);
    UnlockChanger(dcr);
    return ok;
  }
#endif

  changer = GetPoolMemory(PM_FNAME);
  LockChanger(dcr);
  changer = edit_device_codes(dcr, changer,
                              dcr->device_resource->changer_command, cmd);
  dir->fsend(T_("3306 Issuing autochanger \"%s\" command.\n"), cmd);

  // Now issue the command
retry_changercmd:
  bpipe = OpenBpipe(changer, timeout, "r");
  if (!bpipe) {
    dir->fsend(T_("3996 Open bpipe failed.\n"));
    goto bail_out; /* TODO: check if we need to return false */
  }

  if (bstrcmp(cmd, "list") || bstrcmp(cmd, "listall")) {
    // Get output from changer
    while (fgets(dir->msg, len, bpipe->rfd)) {
      dir->message_length = strlen(dir->msg);
      Dmsg1(100, "<stored: %s", dir->msg);
      BnetSend(dir);
    }
  } else if (bstrcmp(cmd, "slots")) {
    slot_number_t slots;
    char buf[100], *p;

    // For slots command, read a single line
    buf[0] = 0;
    fgets(buf, sizeof(buf) - 1, bpipe->rfd);
    buf[sizeof(buf) - 1] = 0;

    // Strip any leading space in front of # of slots
    for (p = buf; B_ISSPACE(*p); p++) {}

    // Validate slot count. If slots == 0 retry retries more times.
    slots = str_to_uint16(p);
    if (slots == 0 && retries-- >= 0) {
      CloseBpipe(bpipe);
      goto retry_changercmd;
    }

    dir->fsend("slots=%hd", slots);
    Dmsg1(100, "<stored: %s\n", dir->msg);
  }

  status = CloseBpipe(bpipe);
  if (status != 0) {
    BErrNo be;
    be.SetErrno(status);
    dir->fsend(T_("3998 Autochanger error: ERR=%s\n"), be.bstrerror());
  }

bail_out:
  UnlockChanger(dcr);
  FreePoolMemory(changer);
  return true;
}

/**
 * Transfer a volume from src_slot to dst_slot.
 * We assume that it is always the Console that is calling us.
 */
bool AutochangerTransferCmd(DeviceControlRecord* dcr,
                            BareosSocket* dir,
                            slot_number_t src_slot,
                            slot_number_t dst_slot)
{
  Device* dev = dcr->dev;
  uint32_t timeout = dcr->device_resource->max_changer_wait;
  POOLMEM* changer;
  Bpipe* bpipe;
  int len = SizeofPoolMemory(dir->msg) - 1;
  int status;

  if (!dev->AttachedToAutochanger() || !dcr->device_resource->changer_name) {
    dir->fsend(T_("3993 Device %s not an autochanger device.\n"),
               dev->print_name());
    return false;
  }

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  if (ChangerUsesNativeScsi(dcr)) {
    LockChanger(dcr);
    bool ok = AutochangerTransferCmdNative(dcr, dir, src_slot, dst_slot);
    UnlockChanger(dcr);
    return ok;
  }
#endif

  if (!dcr->device_resource->changer_command) {
    dir->fsend(T_("3993 Device %s not an autochanger device.\n"),
               dev->print_name());
    return false;
  }

  changer = GetPoolMemory(PM_FNAME);
  LockChanger(dcr);
  changer = transfer_edit_device_codes(dcr, changer,
                                       dcr->device_resource->changer_command,
                                       "transfer", src_slot, dst_slot);
  dir->fsend(T_("3306 Issuing autochanger transfer command.\n"));

  // Now issue the command
  bpipe = OpenBpipe(changer, timeout, "r");
  if (!bpipe) {
    dir->fsend(T_("3996 Open bpipe failed.\n"));
    goto bail_out; /* TODO: check if we need to return false */
  }

  // Get output from changer
  while (fgets(dir->msg, len, bpipe->rfd)) {
    dir->message_length = strlen(dir->msg);
    Dmsg1(100, "<stored: %s\n", dir->msg);
    BnetSend(dir);
  }

  status = CloseBpipe(bpipe);
  if (status != 0) {
    BErrNo be;
    be.SetErrno(status);
    dir->fsend(T_("3998 Autochanger error: ERR=%s\n"), be.bstrerror());
  } else {
    dir->fsend(
        T_("3308 Successfully transferred volume from slot %hd to %hd.\n"),
        src_slot, dst_slot);
  }

bail_out:
  UnlockChanger(dcr);
  FreePoolMemory(changer);
  return true;
}

/**
 * Special version of edit_device_codes for use with transfer subcommand.
 * Edit codes into ChangerCommand
 *  %% = %
 *  %a = destination slot
 *  %c = changer device name
 *  %d = none
 *  %f = none
 *  %j = none
 *  %o = command
 *  %s = source slot
 *  %S = source slot
 *  %v = none
 *
 *  omsg = edited output message
 *  imsg = input string containing edit codes (%x)
 *  cmd = command string (transfer)
 */
static char* transfer_edit_device_codes(DeviceControlRecord* dcr,
                                        POOLMEM*& omsg,
                                        const char* imsg,
                                        const char* cmd,
                                        slot_number_t src_slot,
                                        slot_number_t dst_slot)
{
  const char* p;
  const char* str;
  char ed1[50];

  *omsg = 0;
  Dmsg1(1800, "transfer_edit_device_codes: %s\n", imsg);
  for (p = imsg; *p; p++) {
    if (*p == '%') {
      switch (*++p) {
        case '%':
          str = "%";
          break;
        case 'a':
          str = edit_int64(dst_slot, ed1);
          break;
        case 'c':
          str = NPRT(dcr->device_resource->changer_name);
          break;
        case 'o':
          str = NPRT(cmd);
          break;
        case 's':
        case 'S':
          str = edit_int64(src_slot, ed1);
          break;
        default:
          continue;
      }
    } else {
      ed1[0] = *p;
      ed1[1] = 0;
      str = ed1;
    }
    Dmsg1(1900, "add_str %s\n", str);
    PmStrcat(omsg, (char*)str);
    Dmsg1(1800, "omsg=%s\n", omsg);
  }
  Dmsg1(800, "omsg=%s\n", omsg);

  return omsg;
}

} /* namespace storagedaemon  */
