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
#include <filesystem>
#include <fstream>

#include "bconfig/bconfig.h"

TEST(Bconfig, LoadsWholeEnvironmentFromConfigRoot)
{
  auto environment
      = bconfig::LoadEnvironment("configs/bareos-configparser-tests");

  ASSERT_NE(nullptr, environment);
  EXPECT_FALSE(environment->id().empty());
  EXPECT_EQ(environment->components().size(), 4U);
  EXPECT_TRUE(environment->issues().empty());
  ASSERT_EQ(environment->components().size(), 4U);
  EXPECT_EQ(environment->components()[0]->name, "bareos-dir");
  EXPECT_EQ(environment->components()[1]->name, "bareos-sd");
  EXPECT_EQ(environment->components()[2]->name, "backup-bareos-test-fd");
  EXPECT_EQ(environment->components()[3]->name, "bareos-mon");

  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "director", "Director",
                                           "bareos-dir"));
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "storage-daemon",
                                           "Storage", "bareos-sd"));
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "file-daemon",
                                           "Client", "backup-bareos-test-fd"));
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "console", "Console",
                                           "bareos-mon"));
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

TEST(Bconfig, CollectsResolvedEnvironmentRelations)
{
  auto environment
      = bconfig::LoadEnvironment("configs/bareos-configparser-tests");

  ASSERT_NE(nullptr, environment);
  EXPECT_FALSE(environment->relations().empty());

  const auto relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.component_id == "director" && entry.source_type == "Job"
               && entry.source_name == "backup-bareos-fd"
               && entry.directive == "Client" && entry.target_type == "Client"
               && entry.target_name == "bareos-fd";
      });
  EXPECT_NE(relation, environment->relations().end());

  const auto storage_device_relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.component_id == "director"
               && entry.source_type == "Storage" && entry.source_name == "File"
               && entry.directive == "Device" && entry.target_type == "Device"
               && entry.target_name == "FileStorage";
      });
  EXPECT_NE(storage_device_relation, environment->relations().end());

  const auto client_auth_relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.component_id == "director" && entry.source_type == "Client"
               && entry.source_name == "bareos-fd"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Director"
               && entry.target_name == "bareos-dir"
               && entry.target_component_id == "file-daemon";
      });
  EXPECT_NE(client_auth_relation, environment->relations().end());

  const auto storage_auth_relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.component_id == "director"
               && entry.source_type == "Storage" && entry.source_name == "File"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Director"
               && entry.target_name == "bareos-dir"
               && entry.target_component_id == "storage-daemon";
      });
  EXPECT_NE(storage_auth_relation, environment->relations().end());

  const auto console_auth_relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.component_id == "console" && entry.source_type == "Console"
               && entry.source_name == "bareos-mon"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Console"
               && entry.target_name == "bareos-mon"
               && entry.target_component_id == "director";
      });
  EXPECT_NE(console_auth_relation, environment->relations().end());
}

TEST(Bconfig, InspectionModeSkipsRuntimeHostnameResolution)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root
      = fs::temp_directory_path() / fs::path("bconfig-hostname-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);

  auto director_conf
      = temp_dir / "bareos-dir.d" / "director" / "bareos-dir.conf";
  std::ofstream out(director_conf);
  ASSERT_TRUE(out.is_open());
  out << "Director {\n"
         "  Name = bareos-dir\n"
         "  DirAddress = host-that-should-not-resolve.invalid\n"
         "  QueryFile = \"/tmp/scripts/query.sql\"\n"
         "  Maximum Concurrent Jobs = 10\n"
         "  Password = \"dir_password\"\n"
         "  Messages = Daemon\n"
         "  Auditing = yes\n"
         "  Working Directory =  \"/tmp/tests/backup-bareos-test/working\"\n"
         "  Port = 42001\n"
         "}\n";
  out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  EXPECT_TRUE(environment->issues().empty());
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "director", "Director",
                                           "bareos-dir"));

  fs::remove_all(temp_dir);
}

TEST(Bconfig, CollectsSchedulerOverrideRelations)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root
      = fs::temp_directory_path() / fs::path("bconfig-schedule-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);

  auto schedule_conf
      = temp_dir / "bareos-dir.d" / "schedule" / "WeeklyCycle.conf";
  std::ofstream out(schedule_conf);
  ASSERT_TRUE(out.is_open());
  out << "Schedule {\n"
         "  Name = \"WeeklyCycle\"\n"
         "  Run = Full Pool=Full Storage=File Messages=Standard 1st sat at "
         "21:00\n"
         "}\n";
  out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  const auto pool_relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.source_type == "Schedule"
               && entry.source_name == "WeeklyCycle"
               && entry.directive == "Run/Pool" && entry.target_type == "Pool"
               && entry.target_name == "Full";
      });
  EXPECT_NE(pool_relation, environment->relations().end());

  const auto storage_relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.source_type == "Schedule"
               && entry.source_name == "WeeklyCycle"
               && entry.directive == "Run/Storage"
               && entry.target_type == "Storage" && entry.target_name == "File";
      });
  EXPECT_NE(storage_relation, environment->relations().end());

  const auto messages_relation
      = std::find_if(environment->relations().begin(),
                     environment->relations().end(), [](const auto& entry) {
                       return entry.source_type == "Schedule"
                              && entry.source_name == "WeeklyCycle"
                              && entry.directive == "Run/Messages"
                              && entry.target_type == "Messages"
                              && entry.target_name == "Standard";
                     });
  EXPECT_NE(messages_relation, environment->relations().end());

  fs::remove_all(temp_dir);
}

TEST(Bconfig, CollectsDirectorStorageAutochangerRelations)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root
      = fs::temp_directory_path() / fs::path("bconfig-autochanger-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);
  fs::create_directories(temp_dir / "bareos-sd.d" / "autochanger");

  auto storage_conf
      = temp_dir / "bareos-dir.d" / "storage" / "TapeLibrary.conf";
  std::ofstream storage_out(storage_conf);
  ASSERT_TRUE(storage_out.is_open());
  storage_out << "Storage {\n"
                 "  Name = TapeLibrary\n"
                 "  Address = localhost\n"
                 "  Password = \"sd_password\"\n"
                 "  Device = TapeLibrary\n"
                 "  AutoChanger = yes\n"
                 "  Media Type = File\n"
                 "  Port = 42003\n"
                 "}\n";
  storage_out.close();

  auto autochanger_conf
      = temp_dir / "bareos-sd.d" / "autochanger" / "TapeLibrary.conf";
  std::ofstream autochanger_out(autochanger_conf);
  ASSERT_TRUE(autochanger_out.is_open());
  autochanger_out << "Autochanger {\n"
                     "  Name = TapeLibrary\n"
                     "  Device = FileStorage\n"
                     "  Changer Device = /dev/null\n"
                     "  Changer Command = \"/bin/true\"\n"
                     "}\n";
  autochanger_out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  const auto relation
      = std::find_if(environment->relations().begin(),
                     environment->relations().end(), [](const auto& entry) {
                       return entry.component_id == "director"
                              && entry.source_type == "Storage"
                              && entry.source_name == "TapeLibrary"
                              && entry.directive == "Device"
                              && entry.target_type == "Autochanger"
                              && entry.target_name == "TapeLibrary";
                     });
  EXPECT_NE(relation, environment->relations().end());

  fs::remove_all(temp_dir);
}

TEST(Bconfig, AuthenticationRelationsRequireMatchingSecrets)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root = fs::temp_directory_path() / fs::path("bconfig-auth-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);

  auto filed_director_conf
      = temp_dir / "bareos-fd.d" / "director" / "bareos-dir.conf";
  std::ofstream out(filed_director_conf);
  ASSERT_TRUE(out.is_open());
  out << "Director {\n"
         "  Name = bareos-dir\n"
         "  Password = \"different-fd-password\"\n"
         "  Description = \"Allow the configured Director to access this file "
         "daemon.\"\n"
         "}\n";
  out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  const auto relation = std::find_if(
      environment->relations().begin(), environment->relations().end(),
      [](const auto& entry) {
        return entry.component_id == "director" && entry.source_type == "Client"
               && entry.source_name == "bareos-fd"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Director"
               && entry.target_name == "bareos-dir"
               && entry.target_component_id == "file-daemon";
      });
  EXPECT_EQ(relation, environment->relations().end());

  fs::remove_all(temp_dir);
}

TEST(Bconfig, CollectsAnonymousConsoleAuthenticationRelation)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root
      = fs::temp_directory_path() / fs::path("bconfig-console-auth-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);

  auto bconsole_conf = temp_dir / "bconsole.conf";
  std::ofstream out(bconsole_conf);
  ASSERT_TRUE(out.is_open());
  out << "Director {\n"
         "  Name = bareos-dir\n"
         "  Password = \"dir_password\"\n"
         "  Port = 42001\n"
         "  Address = localhost\n"
         "}\n"
         "\n"
         "Console {\n"
         "  Name = bareos-mon\n"
         "  Description = \"Restricted console used by tray-monitor to get the "
         "status of the director.\"\n"
         "  Password = \"mon_password\"\n"
         "}\n";
  out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  const auto relation
      = std::find_if(environment->relations().begin(),
                     environment->relations().end(), [](const auto& entry) {
                       return entry.component_id == "console"
                              && entry.source_type == "Director"
                              && entry.source_name == "bareos-dir"
                              && entry.directive == "Authentication/Password"
                              && entry.target_type == "Director"
                              && entry.target_name == "bareos-dir"
                              && entry.target_component_id == "director";
                     });
  EXPECT_NE(relation, environment->relations().end());

  fs::remove_all(temp_dir);
}

TEST(Bconfig, LoadsTrayMonitorComponentWhenConfigPresent)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root = fs::temp_directory_path() / fs::path("bconfig-tray-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);

  auto tray_conf = temp_dir / "tray-monitor.conf";
  std::ofstream out(tray_conf);
  ASSERT_TRUE(out.is_open());
  out << "Monitor {\n"
         "  Name = bareos-mon\n"
         "  Password = \"mon_password\"\n"
         "}\n"
         "\n"
         "Director {\n"
         "  Name = bareos-dir\n"
         "  Address = localhost\n"
         "  Port = 42001\n"
         "}\n"
         "\n"
         "Client {\n"
         "  Name = bareos-fd\n"
         "  Address = localhost\n"
         "  Password = \"fd_password\"\n"
         "  Port = 42002\n"
         "}\n"
         "\n"
         "Storage {\n"
         "  Name = File\n"
         "  Address = localhost\n"
         "  Password = \"sd_password\"\n"
         "  Port = 42003\n"
         "}\n";
  out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "tray-monitor",
                                           "Monitor", "bareos-mon"));
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "tray-monitor",
                                           "Director", "bareos-dir"));
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "tray-monitor",
                                           "Client", "bareos-fd"));
  EXPECT_NE(nullptr, bconfig::FindResource(*environment, "tray-monitor",
                                           "Storage", "File"));

  fs::remove_all(temp_dir);
}

TEST(Bconfig, CollectsTrayMonitorAuthenticationRelations)
{
  namespace fs = std::filesystem;

  auto fixture = fs::path("configs/bareos-configparser-tests");
  auto temp_root
      = fs::temp_directory_path() / fs::path("bconfig-tray-auth-XXXXXX");
  std::string temp_template = temp_root.string();
  ASSERT_NE(nullptr, mkdtemp(temp_template.data()));
  fs::path temp_dir(temp_template);

  fs::copy(fixture, temp_dir, fs::copy_options::recursive);

  auto tray_conf = temp_dir / "tray-monitor.conf";
  std::ofstream tray_out(tray_conf);
  ASSERT_TRUE(tray_out.is_open());
  tray_out << "Monitor {\n"
              "  Name = bareos-mon\n"
              "  Password = \"mon_password\"\n"
              "}\n"
              "\n"
              "Director {\n"
              "  Name = bareos-dir\n"
              "  Address = localhost\n"
              "  Port = 42001\n"
              "}\n"
              "\n"
              "Client {\n"
              "  Name = bareos-fd\n"
              "  Address = localhost\n"
              "  Password = \"fd_password\"\n"
              "  Port = 42002\n"
              "}\n"
              "\n"
              "Storage {\n"
              "  Name = File\n"
              "  Address = localhost\n"
              "  Password = \"sd_password\"\n"
              "  Port = 42003\n"
              "}\n";
  tray_out.close();

  auto filed_director_conf
      = temp_dir / "bareos-fd.d" / "director" / "bareos-mon.conf";
  std::ofstream filed_out(filed_director_conf);
  ASSERT_TRUE(filed_out.is_open());
  filed_out << "Director {\n"
               "  Name = bareos-mon\n"
               "  Password = \"fd_password\"\n"
               "  Description = \"Allow tray monitor access to this file "
               "daemon.\"\n"
               "}\n";
  filed_out.close();

  auto stored_director_conf
      = temp_dir / "bareos-sd.d" / "director" / "bareos-mon.conf";
  std::ofstream stored_out(stored_director_conf);
  ASSERT_TRUE(stored_out.is_open());
  stored_out << "Director {\n"
                "  Name = bareos-mon\n"
                "  Password = \"sd_password\"\n"
                "  Description = \"Allow tray monitor access to this storage "
                "daemon.\"\n"
                "}\n";
  stored_out.close();

  auto environment = bconfig::LoadEnvironment(temp_dir.c_str());

  ASSERT_NE(nullptr, environment);
  const auto director_relation = std::find_if(environment->relations().begin(),
                                              environment->relations().end(),
                                              [](const auto& entry) {
                                                return entry.component_id == "tray-monitor"
               && entry.source_type == "Monitor"
               && entry.source_name == "bareos-mon"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Console"
               && entry.target_name == "bareos-mon"
               && entry.target_component_id == "director";
                                              });
  EXPECT_NE(director_relation, environment->relations().end());

  const auto client_relation = std::find_if(environment->relations().begin(),
                                            environment->relations().end(),
                                            [](const auto& entry) {
                                              return entry.component_id == "tray-monitor"
               && entry.source_type == "Client"
               && entry.source_name == "bareos-fd"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Director"
               && entry.target_name == "bareos-mon"
               && entry.target_component_id == "file-daemon";
                                            });
  EXPECT_NE(client_relation, environment->relations().end());

  const auto storage_relation = std::find_if(environment->relations().begin(),
                                             environment->relations().end(),
                                             [](const auto& entry) {
                                               return entry.component_id == "tray-monitor"
               && entry.source_type == "Storage"
               && entry.source_name == "File"
               && entry.directive == "Authentication/Password"
               && entry.target_type == "Director"
               && entry.target_name == "bareos-mon"
               && entry.target_component_id == "storage-daemon";
                                             });
  EXPECT_NE(storage_relation, environment->relations().end());

  fs::remove_all(temp_dir);
}
