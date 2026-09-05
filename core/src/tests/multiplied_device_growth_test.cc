/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2013-2026 Bareos GmbH & Co. KG

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
/*
 * Tests that the implicit autochanger created for a Device with a
 * "Count" directive grows its devices lazily, on demand, up to the
 * configured cap, instead of eagerly pre-creating all of them at
 * config-parse time.
 */

#include "gtest/gtest.h"
#include "include/bareos.h"

#include <chrono>
#include <future>
#include <thread>

#define STORAGE_DAEMON 1
#include "include/jcr.h"
#include "lib/crypto_cache.h"
#include "lib/edit.h"
#include "lib/parse_conf.h"
#include "stored/device_control_record.h"
#include "stored/stored_jcr_impl.h"
#include "stored/job.h"
#include "stored/sd_plugins.h"
#include "stored/sd_stats.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/wait.h"
#include "stored/sd_backends.h"

#include "bsock_mock.h"

using ::testing::Assign;
using ::testing::DoAll;
using ::testing::Return;
using namespace storagedaemon;

namespace storagedaemon {
/* import this to parse the config */
extern bool ParseSdConfig(const char* configfile, int exit_code);
}  // namespace storagedaemon

class MultipliedDeviceGrowthTest : public ::testing::Test {
  void SetUp() override;
  void TearDown() override;
};

void MultipliedDeviceGrowthTest::SetUp()
{
  OSDependentInit();

  /* configfile is a global char* from stored_globals.h */
  configfile = strdup("configs/multiplied_device_growth/");
  my_config = InitSdConfig(configfile, M_CONFIG_ERROR);
  ParseSdConfig(configfile, M_CONFIG_ERROR);
  /* we do not run CheckResources() here, so take care the test configuration
   * is not broken. Also autochangers will not work. */

  InitReservationsLock();
  CreateVolumeLists();
}
void MultipliedDeviceGrowthTest::TearDown()
{
  FreeVolumeLists();

  {
    DeviceResource* d = nullptr;
    foreach_res (d, R_DEVICE) {
      Dmsg1(10, "Term device %s\n", d->archive_device_string);
      if (d->dev) {
        d->dev->ClearVolhdr();
        delete d->dev;
        d->dev = nullptr;
      }
    }
  }

  if (configfile) { free(configfile); }
  if (my_config) { delete my_config; }

  TermReservationsLock();
}

/* wrap JobControlRecord into something we can put into a unique_ptr */
struct GrowthTestJob {
  JobControlRecord* jcr;

  GrowthTestJob() = delete;
  GrowthTestJob(uint32_t jobid)
  {
    jcr = NewStoredJcr();
    jcr->JobId = jobid;
    jcr->sd_auth_key = strdup("no key set");
  }

  ~GrowthTestJob()
  {
    // set jobid = 0 before Free, so we don't try to
    // write a status file
    jcr->JobId = 0;

    // remove sockets so FreeJcr() doesn't clean up memory it doesn't own
    jcr->dir_bsock = nullptr;
    jcr->store_bsock = nullptr;
    jcr->file_bsock = nullptr;

    FreeJcr(jcr);
  }
};

void GrowthWaitThenUnreserve(std::unique_ptr<GrowthTestJob>&);
void GrowthWaitThenUnreserve(std::unique_ptr<GrowthTestJob>& job)
{
  std::this_thread::sleep_for(std::chrono::seconds(5));
  job->jcr->sd_impl->dcr->UnreserveDevice();
  ReleaseDeviceCond();
}

// A single job reserving the implicit autochanger created for "GrowDevice"
// must grow a new device ("GrowDevice0001") on demand, since "GrowDevice0000"
// is not autoselectable.
TEST_F(MultipliedDeviceGrowthTest, single_job_grows_first_device)
{
  auto bsock = std::make_unique<BareosSocketMock>();
  auto job1 = std::make_unique<GrowthTestJob>(111u);
  job1->jcr->dir_bsock = bsock.get();

  EXPECT_CALL(*bsock, recv())
      .WillOnce(BSOCK_RECV(bsock.get(),
                           "use storage=sssss media_type=File pool_name=ppppp "
                           "pool_type=ptptp append=1 copy=0 stripe=0"))
      .WillOnce(BSOCK_RECV(bsock.get(), "use device=GrowDevice"))
      .WillOnce(Return(BNET_EOD))  // end of device commands
      .WillOnce(Return(BNET_EOD))  // end of storage command
      .WillOnce(BSOCK_RECV(
          bsock.get(),
          "1901 No Media."));  // response to DirFindNextAppendableVolume

  EXPECT_CALL(*bsock, send()).WillRepeatedly(Return(true));

  bsock->recv();
  ASSERT_EQ(use_cmd(job1->jcr), true);
  ASSERT_STREQ(bsock->msg, "3000 OK use device device=GrowDevice0001\n");
}

// Two concurrent jobs must each grow a distinct device ("...0001",
// "...0002") without waiting for each other, since Count=2 allows two
// autoselectable devices to be spawned.
TEST_F(MultipliedDeviceGrowthTest, two_concurrent_jobs_grow_two_devices)
{
  auto bsock1 = std::make_unique<BareosSocketMock>();
  auto bsock2 = std::make_unique<BareosSocketMock>();
  auto job1 = std::make_unique<GrowthTestJob>(111u);
  auto job2 = std::make_unique<GrowthTestJob>(222u);
  job1->jcr->dir_bsock = bsock1.get();
  job2->jcr->dir_bsock = bsock2.get();

  EXPECT_CALL(*bsock1, recv())
      .WillOnce(BSOCK_RECV(bsock1.get(),
                           "use storage=sssss media_type=File pool_name=ppppp "
                           "pool_type=ptptp append=1 copy=0 stripe=0"))
      .WillOnce(BSOCK_RECV(bsock1.get(), "use device=GrowDevice"))
      .WillOnce(Return(BNET_EOD))  // end of device commands
      .WillOnce(Return(BNET_EOD))  // end of storage command
      .WillOnce(BSOCK_RECV(
          bsock1.get(),
          "1901 No Media."));  // response to DirFindNextAppendableVolume
  EXPECT_CALL(*bsock1, send()).WillRepeatedly(Return(true));

  EXPECT_CALL(*bsock2, recv())
      .WillOnce(BSOCK_RECV(bsock2.get(),
                           "use storage=sssss media_type=File pool_name=ppppp "
                           "pool_type=ptptp append=1 copy=0 stripe=0"))
      .WillOnce(BSOCK_RECV(bsock2.get(), "use device=GrowDevice"))
      .WillOnce(Return(BNET_EOD))  // end of device commands
      .WillOnce(Return(BNET_EOD))  // end of storage command
      .WillOnce(BSOCK_RECV(
          bsock2.get(),
          "1901 No Media."));  // response to DirFindNextAppendableVolume
  EXPECT_CALL(*bsock2, send()).WillRepeatedly(Return(true));

  bsock1->recv();
  ASSERT_EQ(use_cmd(job1->jcr), true);
  ASSERT_STREQ(bsock1->msg, "3000 OK use device device=GrowDevice0001\n");

  bsock2->recv();
  ASSERT_EQ(use_cmd(job2->jcr), true);
  ASSERT_STREQ(bsock2->msg, "3000 OK use device device=GrowDevice0002\n");
}

// A third concurrent job must not grow a third device, since Count=2 caps
// the maximum number of devices at "0000" (non-autoselect) + "0001" +
// "0002". It must instead wait until one of the two busy devices is freed.
TEST_F(MultipliedDeviceGrowthTest, third_job_waits_for_cap)
{
  auto bsock = std::make_unique<BareosSocketMock>();
  auto job1 = std::make_unique<GrowthTestJob>(111u);
  auto job2 = std::make_unique<GrowthTestJob>(222u);
  auto job3 = std::make_unique<GrowthTestJob>(333u);
  job1->jcr->dir_bsock = job2->jcr->dir_bsock = job3->jcr->dir_bsock
      = bsock.get();

  EXPECT_CALL(*bsock, recv())
      .WillOnce(BSOCK_RECV(bsock.get(),
                           "use storage=sssss media_type=File pool_name=ppppp "
                           "pool_type=ptptp append=1 copy=0 stripe=0"))
      .WillOnce(BSOCK_RECV(bsock.get(), "use device=GrowDevice"))
      .WillOnce(Return(BNET_EOD))  // end of device commands
      .WillOnce(Return(BNET_EOD))  // end of storage command
      .WillOnce(BSOCK_RECV(
          bsock.get(),
          "1901 No Media."))  // response to DirFindNextAppendableVolume
      .WillOnce(BSOCK_RECV(bsock.get(),
                           "use storage=sssss media_type=File pool_name=ppppp "
                           "pool_type=ptptp append=1 copy=0 stripe=0"))
      .WillOnce(BSOCK_RECV(bsock.get(), "use device=GrowDevice"))
      .WillOnce(Return(BNET_EOD))  // end of device commands
      .WillOnce(Return(BNET_EOD))  // end of storage command
      .WillOnce(BSOCK_RECV(
          bsock.get(),
          "1901 No Media."))  // response to DirFindNextAppendableVolume
      .WillOnce(BSOCK_RECV(bsock.get(),
                           "use storage=sssss media_type=File pool_name=ppppp "
                           "pool_type=ptptp append=1 copy=0 stripe=0"))
      .WillOnce(BSOCK_RECV(bsock.get(), "use device=GrowDevice"))
      .WillOnce(Return(BNET_EOD))  // end of device commands
      .WillOnce(Return(BNET_EOD))  // end of storage command
      .WillOnce(BSOCK_RECV(
          bsock.get(),
          "1901 No Media."));  // response to DirFindNextAppendableVolume

  EXPECT_CALL(*bsock, send()).WillRepeatedly(Return(true));

  bsock->recv();
  ASSERT_EQ(use_cmd(job1->jcr), true);
  ASSERT_STREQ(bsock->msg, "3000 OK use device device=GrowDevice0001\n");

  bsock->recv();
  ASSERT_EQ(use_cmd(job2->jcr), true);
  ASSERT_STREQ(bsock->msg, "3000 OK use device device=GrowDevice0002\n");

  /* Both spawnable devices (0001, 0002) are now busy and Count=2 forbids
     growing a third one, so job3 must wait until one is released. */
  auto future = std::async(std::launch::async, [&job3, &bsock] {
    bsock->recv();
    ASSERT_EQ(use_cmd(job3->jcr), true);
    ASSERT_STREQ(bsock->msg, "3000 OK use device device=GrowDevice0001\n");
  });

  // Unreserve job1 (device GrowDevice0001) after waiting for a bit
  auto _ = std::async(std::launch::async,
                       [&job1] { GrowthWaitThenUnreserve(job1); });

  future.wait();
}
