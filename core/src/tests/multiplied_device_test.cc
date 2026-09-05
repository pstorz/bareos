/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2019-2026 Bareos GmbH & Co. KG

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

#include "lib/alist.h"
#include "lib/parse_conf.h"
#include "stored/stored_conf.h"
#include "stored/stored_globals.h"
#include <iostream>

using namespace storagedaemon;

typedef std::unique_ptr<ConfigurationParser> PConfigParser;

static void InitGlobals()
{
  my_config = nullptr;
  OSDependentInit();
}

static DeviceResource* GetDeviceResourceByName(ConfigurationParser& config,
                                               const char* name)
{
  BareosResource* p = config.GetResWithName(R_DEVICE, name, false);
  std::cout << name << p << std::endl;
  return dynamic_cast<DeviceResource*>(p);
}

static DeviceResource* GetMultipliedDeviceResource(ConfigurationParser& config)
{
  const char* name = "$MultipliedDeviceResource";

  BareosResource* p = config.GetResWithName(R_DEVICE, name, false);
  DeviceResource* d = dynamic_cast<DeviceResource*>(p);
  if (d && d->count) { return d; }

  return nullptr;
}

TEST(sd, MultipliedDeviceTest_ConfigParameter)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());
  auto d = GetMultipliedDeviceResource(*test_config);
  ASSERT_TRUE(d);

  EXPECT_EQ(d->count, 3);
}

static uint32_t CountAllDeviceResources(ConfigurationParser& config)
{
  uint32_t count = 0;
  BareosResource* p = nullptr;
  while ((p = config.GetNextRes(R_DEVICE, p))) {
    DeviceResource* d = dynamic_cast<DeviceResource*>(p);
    if (d && d->multiplied_device_resource) { count++; }
  }
  return count;
}

TEST(sd, MultipliedDeviceTest_ImplicitAutochangerCreation)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();
  ASSERT_TRUE(test_config->ParseConfig());

  DeviceResource* explicit_device
      = GetDeviceResourceByName(*test_config, "$DeviceWithExplicitAutochanger");
  ASSERT_TRUE(explicit_device);
  ASSERT_TRUE(explicit_device->changer_res);
  DeviceResource* implicit_device
      = GetDeviceResourceByName(*test_config, "$DeviceWithImplicitAutochanger");
  ASSERT_TRUE(implicit_device);
  ASSERT_TRUE(implicit_device->changer_res);

  EXPECT_EQ(std::string(explicit_device->changer_res->resource_name_),
            "ExplicitAutochanger");
  EXPECT_EQ(std::string(implicit_device->changer_res->resource_name_),
            "DeviceWithImplicitAutochanger");
}

TEST(sd, MultipliedDeviceTest_CountAllAutomaticallyCreatedResources)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());
  auto count = CountAllDeviceResources(*test_config);

  /* Right after ParseConfig(), only the eagerly created "0000" device exists
   * for each of the four Count-configured devices (MultipliedDeviceResource,
   * AnotherMultipliedDeviceResource, DeviceWithExplicitAutochanger,
   * DeviceWithImplicitAutochanger). Further multiplied devices are created
   * lazily on demand at reservation time. */
  int amount_to_check = 4;
  EXPECT_EQ(count, amount_to_check);
}

static uint32_t CheckNamesOfConfiguredDeviceResources_1(
    ConfigurationParser& config)
{
  uint32_t count_str_ok = 0;
  uint32_t count_devices = 0;

  DeviceResource* source_device_resource
      = GetDeviceResourceByName(config, "$MultipliedDeviceResource");
  if (!source_device_resource) { return 0; }

  /* Right after ParseConfig(), only the eagerly created "0000" device
   * exists; further multiplied devices ("0001", "0002", ...) are created
   * lazily on demand at reservation time (see reserve.cc
   * SpawnMultipliedDevice()), which is exercised separately by
   * multiplied_device_growth_test.cc. */
  BareosResource* p = nullptr;
  while ((p = config.GetNextRes(R_DEVICE, p))) {
    DeviceResource* device = dynamic_cast<DeviceResource*>(p);
    if (device->multiplied_device_resource == source_device_resource) {
      ++count_devices;
      if (std::string(device->resource_name_)
          == "MultipliedDeviceResource0000") {
        ++count_str_ok;
      }
    } /* if (device->multiplied_device_resource) */
  } /* while GetNextRes */
  return (count_devices == 1) ? count_str_ok : 0;
}

TEST(sd, MultipliedDeviceTest_CheckNames_1)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());

  auto count = CheckNamesOfConfiguredDeviceResources_1(*test_config);

  EXPECT_EQ(count, 1);
}

static uint32_t CheckNamesOfConfiguredDeviceResources_2(
    ConfigurationParser& config)
{
  uint32_t count_str_ok = 0;
  uint32_t count_devices = 0;

  DeviceResource* source_device_resource
      = GetDeviceResourceByName(config, "$AnotherMultipliedDeviceResource");
  if (!source_device_resource) { return 0; }

  /* Right after ParseConfig(), only the eagerly created "0000" device
   * exists, even though Count=100 allows up to 101 devices to be grown
   * lazily on demand at reservation time. */
  BareosResource* p = nullptr;
  while ((p = config.GetNextRes(R_DEVICE, p))) {
    DeviceResource* device_resource = dynamic_cast<DeviceResource*>(p);
    if (device_resource->multiplied_device_resource == source_device_resource) {
      ++count_devices;
      if (std::string(device_resource->resource_name_)
          == "AnotherMultipliedDeviceResource0000") {
        ++count_str_ok;
      }
    } /* if (device_resource->multiplied_device_resource) */
  } /* while GetNextRes */
  return (count_devices == 1) ? count_str_ok : 0;
}

TEST(sd, MultipliedDeviceTest_CheckNames_2)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());

  auto count = CheckNamesOfConfiguredDeviceResources_2(*test_config);

  EXPECT_EQ(count, 1);
}

static uint32_t CheckAutochangerInAllDevices(ConfigurationParser& config)
{
  /* Right after ParseConfig(), only the eagerly created "0000" device of
   * each multiplied device is attached to its implicit autochanger. */
  std::map<std::string, std::string> names = {
      {"MultipliedDeviceResource0000", "virtual-multiplied-device-autochanger"},
      {"AnotherMultipliedDeviceResource0000",
       "another-virtual-multiplied-device-autochanger"}};

  uint32_t count_str_ok = 0;
  BareosResource* p = nullptr;

  while ((p = config.GetNextRes(R_DEVICE, p))) {
    DeviceResource* device = dynamic_cast<DeviceResource*>(p);
    if (device && device->multiplied_device_resource) {
      if (device->changer_res && device->changer_res->resource_name_) {
        std::string changer_name(device->changer_res->resource_name_);
        if (names.find(device->resource_name_) != names.end()) {
          if (names.at(device->resource_name_) == changer_name) {
            ++count_str_ok;
          }
        }
      }
    }
  }
  return count_str_ok;
}

TEST(sd, MultipliedDeviceTest_CheckNameOfAutomaticallyAttachedAutochanger)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());

  auto count = CheckAutochangerInAllDevices(*test_config);

  EXPECT_EQ(count, 2);
}

static uint32_t CheckSomeDevicesInAutochanger(ConfigurationParser& config)
{
  uint32_t count_str_ok = 0;
  BareosResource* p = nullptr;

  /* Right after ParseConfig(), only the eagerly created "0000" device is
   * attached to the implicit autochanger; further devices are attached
   * lazily on demand at reservation time. */
  std::set<std::string> names = {{"MultipliedDeviceResource0000"}};

  while ((p = config.GetNextRes(R_AUTOCHANGER, p))) {
    AutochangerResource* autochanger = dynamic_cast<AutochangerResource*>(p);
    if (autochanger && autochanger->device_resources) {
      std::string autochanger_name(autochanger->resource_name_);
      std::string autochanger_name_test(
          "virtual-multiplied-device-autochanger");
      if (autochanger_name == autochanger_name_test) {
        for (auto* d : autochanger->device_resources) {
          std::string device_name(d->resource_name_);
          if (names.find(device_name) != names.end()) { ++count_str_ok; }
        }
      }
    }
  }
  return count_str_ok;
}

TEST(sd,
     MultipliedDeviceTest_CheckNameOfDevicesAutomaticallyAttachedToAutochanger)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());

  auto count = CheckSomeDevicesInAutochanger(*test_config);

  EXPECT_EQ(count, 1);
}

TEST(sd, MultipliedDeviceTest_CheckPointerReferenceOfCopiedDevice)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());

  BareosResource* p;
  p = test_config->GetResWithName(R_DEVICE, "$MultipliedDeviceResource");
  ASSERT_TRUE(p);
  DeviceResource* original_device_resource = dynamic_cast<DeviceResource*>(p);
  p = test_config->GetResWithName(R_DEVICE, "MultipliedDeviceResource0000");
  ASSERT_TRUE(p);
  DeviceResource* multiplied_device_resource = dynamic_cast<DeviceResource*>(p);
  EXPECT_EQ(original_device_resource,
            multiplied_device_resource->multiplied_device_resource);
}

TEST(sd, MultipliedDeviceTest_CheckMultipliedDeviceTemplatePointer)
{
  InitGlobals();
  std::string path_to_config = "configs/stored_multiplied_device/";

  PConfigParser test_config(InitSdConfig(path_to_config.c_str(), M_INFO));
  storagedaemon::my_config = test_config.get();

  ASSERT_TRUE(test_config->ParseConfig());

  BareosResource* p;
  p = test_config->GetResWithName(R_DEVICE, "$MultipliedDeviceResource");
  ASSERT_TRUE(p);
  DeviceResource* original_device_resource = dynamic_cast<DeviceResource*>(p);
  ASSERT_TRUE(original_device_resource->changer_res);

  // the implicit autochanger must be able to find its way back to the
  // template device it should lazily grow further devices from
  EXPECT_EQ(original_device_resource->changer_res->multiplied_device_template,
            original_device_resource);
  EXPECT_EQ(original_device_resource->next_multiplied_device_index, 1u);
}
