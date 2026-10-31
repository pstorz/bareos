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

#ifndef BAREOS_INCLUDE_DIR_SD_PROTOCOL_H_
#define BAREOS_INCLUDE_DIR_SD_PROTOCOL_H_

#include <optional>
#include <string_view>

/* Version of the job protocol between director and storage daemon.
 * Increment it on every incompatible change; director and storage daemon
 * only run jobs together if both use the same version.
 *   1: FindMedia and GetVolInfo carry the device and drive index
 *   2: inventory command, director orders the devices to reserve
 *   3: volume info tells the storage daemon to rewrite the label
 *   4: drive events report autochanger loads and unloads
 *   5: UpdateMedia reports a volume event instead of status, slot and
 *      InChanger
 *   6: the inventory starts with the epoch of the storage daemon
 *   7: volume info carries the changer action the director decided */
inline constexpr int kDirSdProtocolVersion = 7;

/* How the requesting drive gets the volume, decided by the director from
 * its storage model and sent with the volume info. */
enum class ChangerAction
{
  kUnknown,   // no decision, the storage daemon acts on its own
  kMounted,   // already in the requesting drive
  kLoad,      // load it from its slot
  kTransfer,  // take it from the idle drive from_drive
  kBusy       // another job uses it in drive from_drive, do not take it
};

struct ChangerInstruction {
  ChangerAction action{ChangerAction::kUnknown};
  int from_drive{-1};
  constexpr bool operator==(const ChangerInstruction&) const = default;
};

constexpr std::string_view ChangerActionName(ChangerAction action)
{
  switch (action) {
    case ChangerAction::kMounted:
      return "mounted";
    case ChangerAction::kLoad:
      return "load";
    case ChangerAction::kTransfer:
      return "transfer";
    case ChangerAction::kBusy:
      return "busy";
    case ChangerAction::kUnknown:
      break;
  }
  return "unknown";
}

constexpr ChangerAction ParseChangerAction(std::string_view name)
{
  for (auto action : {ChangerAction::kMounted, ChangerAction::kLoad,
                      ChangerAction::kTransfer, ChangerAction::kBusy}) {
    if (ChangerActionName(action) == name) { return action; }
  }
  return ChangerAction::kUnknown;
}

/* May the storage daemon move the volume it finds in another drive of the
 * autochanger (drive index other_drive) into the requesting drive? Only
 * if the director asked for exactly that transfer, or decided nothing. */
constexpr bool MayTakeVolumeFromDrive(const ChangerInstruction& instruction,
                                      int other_drive)
{
  switch (instruction.action) {
    case ChangerAction::kUnknown:
      return true;
    case ChangerAction::kTransfer:
      return instruction.from_drive == other_drive;
    default:
      return false;
  }
}

/* What the storage daemon observed on a volume. It reports facts only;
 * the director decides the resulting volume status. */
enum class VolumeEvent
{
  kNone,
  kLabeled,
  kFull,
  kError
};

constexpr std::string_view VolumeEventName(VolumeEvent event)
{
  switch (event) {
    case VolumeEvent::kLabeled:
      return "labeled";
    case VolumeEvent::kFull:
      return "full";
    case VolumeEvent::kError:
      return "error";
    case VolumeEvent::kNone:
      break;
  }
  return "none";
}

constexpr std::optional<VolumeEvent> ParseVolumeEvent(std::string_view name)
{
  for (auto event : {VolumeEvent::kNone, VolumeEvent::kLabeled,
                     VolumeEvent::kFull, VolumeEvent::kError}) {
    if (VolumeEventName(event) == name) { return event; }
  }
  return std::nullopt;
}

/* The storage daemon marks a volume Full or Error locally to stop
 * writing to it; report that observation to the director. */
constexpr VolumeEvent VolumeEventFromLocalStatus(std::string_view status,
                                                 bool labeled)
{
  if (labeled) { return VolumeEvent::kLabeled; }
  if (status == "Error") { return VolumeEvent::kError; }
  if (status == "Full") { return VolumeEvent::kFull; }
  return VolumeEvent::kNone;
}

enum class JobCommandCheck
{
  kOk,
  kVersionMismatch,
  kMalformed
};

/* Classify a parsed job command; fields is the bsscanf result, which
 * is -1 for a director that does not send SdProtocolVersion at all. */
constexpr JobCommandCheck CheckJobCommand(std::string_view cmd,
                                          int fields,
                                          int expected_fields,
                                          int version)
{
  if (cmd.find(" SdProtocolVersion=") == std::string_view::npos) {
    return JobCommandCheck::kVersionMismatch;
  }
  if (fields != expected_fields) { return JobCommandCheck::kMalformed; }
  return version == kDirSdProtocolVersion ? JobCommandCheck::kOk
                                          : JobCommandCheck::kVersionMismatch;
}

#endif  // BAREOS_INCLUDE_DIR_SD_PROTOCOL_H_
