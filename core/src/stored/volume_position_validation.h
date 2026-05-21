/*
   BAREOS® - Backup Archiving REcovery Open Sourced

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
#ifndef BAREOS_CORE_SRC_STORED_VOLUME_POSITION_VALIDATION_H_
#define BAREOS_CORE_SRC_STORED_VOLUME_POSITION_VALIDATION_H_

#include <cstdint>

namespace storagedaemon {

enum class FileBlockCatalogRelation
{
  kMatch,
  kDeviceAhead,
  kCatalogAhead,
};

struct FileBlockCatalogPosition {
  uint32_t files;
  uint32_t blocks;
};

constexpr FileBlockCatalogRelation CompareEodFileBlockPosition(
    uint32_t catalog_files,
    uint32_t device_files)
{
  if (catalog_files == device_files) { return FileBlockCatalogRelation::kMatch; }

  return device_files > catalog_files ? FileBlockCatalogRelation::kDeviceAhead
                                      : FileBlockCatalogRelation::kCatalogAhead;
}

constexpr FileBlockCatalogPosition CorrectCatalogForEodFileMismatch(
    FileBlockCatalogPosition catalog_position,
    uint32_t device_files)
{
  catalog_position.files = device_files;

  // FILE_BLOCK devices track the current block inside the current file, while
  // VolCatBlocks is cumulative for the whole volume. Preserve the cumulative
  // catalog value here instead of overwriting it with a per-file block index.
  return catalog_position;
}

}  // namespace storagedaemon

#endif  // BAREOS_CORE_SRC_STORED_VOLUME_POSITION_VALIDATION_H_
