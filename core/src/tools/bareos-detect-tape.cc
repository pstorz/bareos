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
 * @file bareos-detect-tape.cc
 * Standalone CLI tool for SCSI tape drive and autochanger detection.
 *
 * Scans /dev/sg* for tape drives and medium changers using SCSI INQUIRY,
 * reads element status (slot inventory, drive states, barcodes) from each
 * discovered changer, and prints the result.  Optionally generates ready-
 * to-paste bareos-sd.conf / bareos-dir.conf config snippets.
 *
 * Requires root (or sg group membership) to open /dev/sg* devices.
 */

#include "include/bareos.h"

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
#  include "lib/scsi_scanner.h"
#  include "lib/scsi_changer.h"
#  include <fcntl.h>
#  include <unistd.h>
#endif

#include <CLI/CLI.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

static void PrintTapeDrives(
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
    const std::vector<bareos::scsi::ScsiDeviceInfo>& devices
#endif
)
{
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  using namespace bareos::scsi;

  bool found = false;
  for (const auto& dev : devices) {
    if (dev.device_type != kScsiTypeTapeDrive) { continue; }
    if (!found) {
      printf("Tape drives:\n");
      found = true;
    }
    printf("  %-12s  %-12s  %-8s  %-16s  SN:%s\n", dev.sg_path.c_str(),
           dev.st_path.empty() ? "(no st)" : dev.st_path.c_str(),
           dev.vendor.c_str(), dev.product.c_str(), dev.serial.c_str());
  }
  if (!found) { printf("Tape drives: none detected\n"); }
#else
  printf("Tape drives: SCSI support not compiled in\n");
#endif
}

static void PrintChangers(
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
    const std::vector<bareos::scsi::ScsiDeviceInfo>& devices,
    const std::vector<bareos::scsi::ScsiDeviceInfo>& drives
#endif
)
{
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  using namespace bareos::scsi;

  bool found = false;
  int changer_idx = 0;

  for (const auto& dev : devices) {
    if (dev.device_type != kScsiTypeChanger) { continue; }

    if (!found) {
      printf("\nChangers:\n");
      found = true;
    }

    printf("  [%d] %s  %-8s  %-16s  SN:%s\n", changer_idx, dev.sg_path.c_str(),
           dev.vendor.c_str(), dev.product.c_str(), dev.serial.c_str());

    int fd = open(dev.sg_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      printf("      (cannot open device: %s)\n", strerror(errno));
      ++changer_idx;
      continue;
    }

    ChangerInventory inv{};
    bool ok = ReadElementStatus(fd, dev.sg_path.c_str(), inv);
    close(fd);

    if (!ok) {
      printf("      (READ ELEMENT STATUS failed)\n");
      ++changer_idx;
      continue;
    }

    printf("      Slots: %u    Drives: %u    I/E slots: %u\n",
           static_cast<unsigned>(inv.num_slots),
           static_cast<unsigned>(inv.num_drives),
           static_cast<unsigned>(inv.num_ie_slots));

    // Print drives — try to match with a scanned standalone tape sg device.
    // Scanned tape drives that follow the changer in /dev/sg* order are
    // typically part of the same library.
    for (const auto& drv : inv.drives) {
      // Find corresponding ScsiDeviceInfo for this drive index if available.
      std::string sg, st;
      if (drv.logical_drive < static_cast<uint16_t>(drives.size())) {
        sg = drives[drv.logical_drive].sg_path;
        st = drives[drv.logical_drive].st_path;
      }

      if (drv.loaded) {
        printf("      Drive %u: [loaded] barcode=%-12s",
               static_cast<unsigned>(drv.logical_drive), drv.barcode);
      } else {
        printf("      Drive %u: [empty ]                    ",
               static_cast<unsigned>(drv.logical_drive));
      }
      if (!sg.empty()) { printf("  sg=%-12s  st=%s", sg.c_str(), st.c_str()); }
      printf("\n");
    }

    for (const auto& slot : inv.slots) {
      if (slot.full) {
        printf("      Slot %3u: [FULL ]  %s\n",
               static_cast<unsigned>(slot.logical_slot), slot.barcode);
      } else {
        printf("      Slot %3u: [empty]\n",
               static_cast<unsigned>(slot.logical_slot));
      }
    }

    for (const auto& ie : inv.ie_slots) {
      if (ie.full) {
        printf("      I/E  %3u: [FULL ]  %s\n",
               static_cast<unsigned>(ie.logical_slot), ie.barcode);
      } else {
        printf("      I/E  %3u: [empty]\n",
               static_cast<unsigned>(ie.logical_slot));
      }
    }

    ++changer_idx;
  }

  if (!found) { printf("\nChangers: none detected\n"); }
#else
  printf("\nChangers: SCSI support not compiled in\n");
#endif
}

// ---------------------------------------------------------------------------
// Config snippet generation
// ---------------------------------------------------------------------------

static void GenerateConfig(
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
    const std::vector<bareos::scsi::ScsiDeviceInfo>& devices,
    const std::vector<bareos::scsi::ScsiDeviceInfo>& drives
#endif
)
{
#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  using namespace bareos::scsi;

  int changer_idx = 0;
  for (const auto& dev : devices) {
    if (dev.device_type != kScsiTypeChanger) { continue; }

    int fd = open(dev.sg_path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      ++changer_idx;
      continue;
    }
    ChangerInventory inv{};
    bool ok = ReadElementStatus(fd, dev.sg_path.c_str(), inv);
    close(fd);
    if (!ok) {
      ++changer_idx;
      continue;
    }

    std::string name = "Autochanger-" + std::to_string(changer_idx);

    printf("\n# ── Generated snippet for %s (%s %s) ──────────────\n",
           dev.sg_path.c_str(), dev.vendor.c_str(), dev.product.c_str());
    printf("# Add to /etc/bareos/bareos-sd.d/autochanger/\n\n");

    // Build device name list
    std::string drive_names;
    for (unsigned i = 0; i < static_cast<unsigned>(inv.num_drives); ++i) {
      if (i > 0) { drive_names += ", "; }
      drive_names += name + "-Drive-" + std::to_string(i);
    }

    printf("Autochanger {\n");
    printf("  Name = \"%s\"\n", name.c_str());
    printf("  UseNativeScsi = yes\n");
    printf("  ChangerDevice = %s\n", dev.sg_path.c_str());
    printf("  Device = %s\n", drive_names.c_str());
    printf("}\n\n");

    for (unsigned i = 0; i < static_cast<unsigned>(inv.num_drives); ++i) {
      std::string st = "<tape-device>";
      if (i < static_cast<unsigned>(drives.size())) {
        if (!drives[i].st_path.empty()) { st = drives[i].st_path; }
      }
      printf("Device {\n");
      printf("  Name = \"%s-Drive-%u\"\n", name.c_str(), i);
      printf("  DeviceType = tape\n");
      printf("  ArchiveDevice = %s\n", st.c_str());
      printf("  MediaType = LTO\n");
      printf("  AutoChanger = yes\n");
      printf("}\n\n");
    }

    printf("# Add to /etc/bareos/bareos-dir.d/storage/\n\n");
    printf("Storage {\n");
    printf("  Name = \"%s\"\n", name.c_str());
    printf("  Address = <bareos-sd-hostname>\n");
    printf("  Password = \"<bareos-sd-password>\"\n");
    printf("  Device = \"%s\"\n", name.c_str());
    printf("  MediaType = LTO\n");
    printf("  AutoChanger = yes\n");
    printf("}\n");

    ++changer_idx;
  }
#else
  printf("Config generation: SCSI support not compiled in\n");
#endif
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
  bool generate = false;

  CLI::App app{"bareos-detect-tape: detect SCSI tape drives and changers"};
  app.add_flag("-g,--generate", generate,
               "Print bareos-sd / bareos-dir config snippets");
  CLI11_PARSE(app, argc, argv);

#ifdef HAVE_LOWLEVEL_SCSI_INTERFACE
  using namespace bareos::scsi;

  std::vector<ScsiDeviceInfo> all = ScanScsiDevices();

  // Split into tape drives and changers for convenience.
  std::vector<ScsiDeviceInfo> tape_drives, changers;
  for (const auto& d : all) {
    if (d.device_type == kScsiTypeTapeDrive) {
      tape_drives.push_back(d);
    } else if (d.device_type == kScsiTypeChanger) {
      changers.push_back(d);
    }
  }

  PrintTapeDrives(all);
  PrintChangers(all, tape_drives);
  if (generate) { GenerateConfig(all, tape_drives); }

  return (tape_drives.empty() && changers.empty()) ? 1 : 0;
#else
  PrintTapeDrives();
  PrintChangers();
  if (generate) { GenerateConfig(); }
  return 1;
#endif
}
