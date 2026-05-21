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
#include "gtest/gtest.h"

#include "stored/volume_position_validation.h"

namespace storagedaemon {
namespace {

TEST(VolumePositionValidationTest, compare_file_block_position_matches)
{
  EXPECT_EQ(CompareEodFileBlockPosition(17, 17),
            FileBlockCatalogRelation::kMatch);
}

TEST(VolumePositionValidationTest, compare_file_block_position_detects_device_ahead)
{
  EXPECT_EQ(CompareEodFileBlockPosition(17, 18),
            FileBlockCatalogRelation::kDeviceAhead);
}

TEST(VolumePositionValidationTest, compare_file_block_position_detects_catalog_ahead)
{
  EXPECT_EQ(CompareEodFileBlockPosition(18, 17),
            FileBlockCatalogRelation::kCatalogAhead);
}

TEST(VolumePositionValidationTest, file_mismatch_correction_preserves_block_count)
{
  auto corrected
      = CorrectCatalogForEodFileMismatch(FileBlockCatalogPosition{526, 4662327},
                                         527);

  EXPECT_EQ(corrected.files, 527u);
  EXPECT_EQ(corrected.blocks, 4662327u);
}

}  // namespace
}  // namespace storagedaemon
