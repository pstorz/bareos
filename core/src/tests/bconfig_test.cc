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

#include "gtest/gtest.h"
#include "include/bareos.h"

#include <algorithm>

#include "bconfig/bconfig.h"

TEST(Bconfig, LoadsWholeEnvironmentFromConfigRoot)
{
  auto environment
      = bconfig::LoadEnvironment("configs/bareos-configparser-tests");

  ASSERT_NE(nullptr, environment);
  EXPECT_FALSE(environment->id().empty());
  EXPECT_EQ(environment->components().size(), 3U);
  EXPECT_TRUE(environment->issues().empty());
  ASSERT_EQ(environment->components().size(), 3U);
  EXPECT_EQ(environment->components()[0]->name, "bareos-dir");
  EXPECT_EQ(environment->components()[1]->name, "bareos-sd");
  EXPECT_EQ(environment->components()[2]->name, "backup-bareos-test-fd");

  EXPECT_NE(nullptr,
            bconfig::FindResource(*environment, "director", "Director",
                                  "bareos-dir"));
  EXPECT_NE(nullptr,
            bconfig::FindResource(*environment, "storage-daemon", "Storage",
                                  "bareos-sd"));
  EXPECT_NE(nullptr,
            bconfig::FindResource(*environment, "file-daemon", "Client",
                                  "backup-bareos-test-fd"));
}

TEST(Bconfig, CollectsStableResourceIdsAcrossEnvironment)
{
  auto environment
      = bconfig::LoadEnvironment("configs/bareos-configparser-tests");

  ASSERT_NE(nullptr, environment);

  size_t resource_count = 0;
  for (const auto& component : environment->components()) {
    EXPECT_FALSE(component->id.empty());
    EXPECT_FALSE(component->resources.empty());
    for (const auto& resource : component->resources) {
      EXPECT_FALSE(resource.id.empty());
      ++resource_count;
    }
  }

  EXPECT_GT(resource_count, 0U);
}
