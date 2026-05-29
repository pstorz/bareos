/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2021-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "gtest/gtest.h"
#else
#  include "gtest/gtest.h"
#  include "include/bareos.h"
#endif


#include <chrono>
#include <filesystem>

#define STORAGE_DAEMON 1
#include "include/jcr.h"
#include "lib/crypto_cache.h"
#include "lib/edit.h"
#include "lib/parse_conf.h"
#include "stored/butil.h"
#include "stored/device_control_record.h"
#include "stored/stored_jcr_impl.h"
#include "stored/job.h"
#include "stored/sd_plugins.h"
#include "stored/sd_stats.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/wait.h"
#include "stored/sd_backends.h"

#define CONFIG_SUBDIR "sd_backend"
#include "sd_backend_tests.h"

using namespace storagedaemon;


// Test that load and unloads a tape device.
TEST_F(sd, backend_load_unload)
{
  const char* name = "sd_backend_test";
  char dev_name[10] = "tape1";

  JobControlRecord* jcr = SetupDummyJcr(name, nullptr, nullptr);
  ASSERT_TRUE(jcr);

  DeviceResource* device_resource
      = (DeviceResource*)my_config->GetResWithName(R_DEVICE, dev_name);

  Device* dev = FactoryCreateDevice(jcr, device_resource);
  ASSERT_TRUE(dev);

  Dmsg0(100, "open\n");
  /* Open device. Calling d_open directly,
   * because otherwise OpenDevice()/open()
   * would also try IOCTLs on the tape device,
   * which will fail on our dummy device. */
#if defined HAVE_MSVC
  dev->fd = dev->d_open("NUL", 0, 0640);
#else
  dev->fd = dev->d_open("/dev/null", 0, 0640);
#endif
  ASSERT_TRUE(dev->fd > 0);

  /* always true on generic (disk) device.
   * On /dev/null used as tape it will fail. */
  ASSERT_FALSE(dev->offline());

  Dmsg0(100, "cleanup dev \n");
  delete dev;

  Dmsg0(100, "cleanup\n");
  FreeJcr(jcr);
}

TEST_F(sd, virtual_tape_persists_filemarks)
{
  const char* name = "sd_backend_test";
  char dev_name[] = "virtual-tape1";

  JobControlRecord* jcr = SetupDummyJcr(name, nullptr, nullptr);
  ASSERT_TRUE(jcr);

  DeviceResource* device_resource
      = (DeviceResource*)my_config->GetResWithName(R_DEVICE, dev_name);
  ASSERT_TRUE(device_resource);

  Device* dev = FactoryCreateDevice(jcr, device_resource);
  ASSERT_TRUE(dev);

  const auto tape_dir = std::filesystem::temp_directory_path()
                        / ("bareos-virtual-tape-" + std::to_string(getpid()));
  std::filesystem::remove_all(tape_dir);

  FreeMemory(dev->archive_device_string);
  dev->archive_device_string = GetMemory(tape_dir.string().size() + 1);
  PmStrcpy(dev->archive_device_string, tape_dir.string().c_str());
  Mmsg(dev->prt_name, "\"%s\" (%s)", device_resource->resource_name_,
       dev->archive_device_string);

  DeviceControlRecord dcr;
  dcr.jcr = jcr;
  dcr.dev = dev;
  dcr.device_resource = device_resource;

  ASSERT_TRUE(dev->open(&dcr, DeviceMode::CREATE_READ_WRITE));

  const char first_block[] = "abc";
  const char second_block[] = "defg";
  ASSERT_EQ(dev->write(first_block, sizeof(first_block) - 1),
            sizeof(first_block) - 1);
  ASSERT_EQ(dev->write(second_block, sizeof(second_block) - 1),
            sizeof(second_block) - 1);
  dev->SetAppend();

  struct mtget status;
  ASSERT_EQ(dev->d_ioctl(dev->fd, MTIOCGET, (char*)&status), 0);
  EXPECT_EQ(status.mt_fileno, 0);
  EXPECT_EQ(status.mt_blkno, 2);

  ASSERT_TRUE(dev->weof(1));
  ASSERT_EQ(dev->d_ioctl(dev->fd, MTIOCGET, (char*)&status), 0);
  EXPECT_EQ(status.mt_fileno, 1);
  EXPECT_EQ(status.mt_blkno, 0);

  ASSERT_TRUE(dev->rewind(&dcr));
  ASSERT_EQ(dev->d_ioctl(dev->fd, MTIOCGET, (char*)&status), 0);
  EXPECT_EQ(status.mt_fileno, 0);
  EXPECT_EQ(status.mt_blkno, 0);

  char buffer[16];
  ASSERT_EQ(dev->read(buffer, sizeof(buffer)), sizeof(first_block) - 1);
  EXPECT_EQ(std::string(buffer, sizeof(first_block) - 1), "abc");
  ASSERT_EQ(dev->read(buffer, sizeof(buffer)), sizeof(second_block) - 1);
  EXPECT_EQ(std::string(buffer, sizeof(second_block) - 1), "defg");
  EXPECT_EQ(dev->read(buffer, sizeof(buffer)), 0);
  EXPECT_EQ(dev->read(buffer, sizeof(buffer)), 0);

  delete dev;
  FreeJcr(jcr);
  std::filesystem::remove_all(tape_dir);
}

TEST_F(sd, virtual_tape_only_advances_despool_job_switches)
{
  const char* name = "sd_backend_test";
  char dev_name[] = "virtual-tape1";

  JobControlRecord* jcr = SetupDummyJcr(name, nullptr, nullptr);
  ASSERT_TRUE(jcr);

  DeviceResource* device_resource
      = (DeviceResource*)my_config->GetResWithName(R_DEVICE, dev_name);
  ASSERT_TRUE(device_resource);

  Device* dev = FactoryCreateDevice(jcr, device_resource);
  ASSERT_TRUE(dev);

  const auto tape_dir
      = std::filesystem::temp_directory_path()
        / ("bareos-virtual-tape-despool-" + std::to_string(getpid()));
  std::filesystem::remove_all(tape_dir);

  FreeMemory(dev->archive_device_string);
  dev->archive_device_string = GetMemory(tape_dir.string().size() + 1);
  PmStrcpy(dev->archive_device_string, tape_dir.string().c_str());
  Mmsg(dev->prt_name, "\"%s\" (%s)", device_resource->resource_name_,
       dev->archive_device_string);

  DeviceControlRecord dcr;
  dcr.jcr = jcr;
  dcr.dev = dev;
  dcr.device_resource = device_resource;

  ASSERT_TRUE(dev->open(&dcr, DeviceMode::CREATE_READ_WRITE));
  dev->attached_dcrs.push_back(&dcr);

  const char direct_first[] = "direct-first";
  ASSERT_EQ(dev->write(direct_first, strlen(direct_first)),
            static_cast<ssize_t>(strlen(direct_first)));

  const char direct_second[] = "direct-second";
  ASSERT_EQ(dev->write(direct_second, strlen(direct_second)),
            static_cast<ssize_t>(strlen(direct_second)));

  struct mtget status;
  ASSERT_EQ(dev->d_ioctl(dev->fd, MTIOCGET, (char*)&status), 0);
  EXPECT_EQ(status.mt_fileno, 0);
  EXPECT_EQ(status.mt_blkno, 2);

  JobControlRecord* despool_jcr_first
      = SetupDummyJcr("sd_backend_test_despool_first", nullptr, nullptr);
  ASSERT_TRUE(despool_jcr_first);
  despool_jcr_first->JobId = 101;

  JobControlRecord* despool_jcr_second
      = SetupDummyJcr("sd_backend_test_despool_second", nullptr, nullptr);
  ASSERT_TRUE(despool_jcr_second);
  despool_jcr_second->JobId = 202;

  DeviceControlRecord despool_dcr_first;
  despool_dcr_first.jcr = despool_jcr_first;
  despool_dcr_first.dev = dev;
  despool_dcr_first.despooling = true;
  dev->attached_dcrs.push_back(&despool_dcr_first);

  const char despool_first[] = "despool-first";
  ASSERT_EQ(dev->write(despool_first, strlen(despool_first)),
            static_cast<ssize_t>(strlen(despool_first)));

  despool_dcr_first.despooling = false;
  DeviceControlRecord despool_dcr_second;
  despool_dcr_second.jcr = despool_jcr_second;
  despool_dcr_second.dev = dev;
  despool_dcr_second.despooling = true;
  dev->attached_dcrs.push_back(&despool_dcr_second);

  const char despool_second[] = "despool-second";
  ASSERT_EQ(dev->write(despool_second, strlen(despool_second)),
            static_cast<ssize_t>(strlen(despool_second)));

  ASSERT_EQ(dev->d_ioctl(dev->fd, MTIOCGET, (char*)&status), 0);
  EXPECT_EQ(status.mt_fileno, 1);
  EXPECT_EQ(status.mt_blkno, 1);

  dev->attached_dcrs.clear();
  delete dev;
  FreeJcr(jcr);
  FreeJcr(despool_jcr_first);
  FreeJcr(despool_jcr_second);
  std::filesystem::remove_all(tape_dir);
}
