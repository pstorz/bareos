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

#ifndef BAREOS_BCONFIG_BCONFIG_H_
#define BAREOS_BCONFIG_BCONFIG_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ConfigurationParser;
class BareosResource;

namespace bconfig {

enum class ComponentKind
{
  kDirector,
  kStorageDaemon,
  kFileDaemon
};

struct EnvironmentResource {
  std::string id;
  ComponentKind component{};
  std::string component_id;
  std::string type;
  std::string group_name;
  std::string name;
  const BareosResource* resource{};
};

struct EnvironmentIssue {
  ComponentKind component{};
  std::string component_id;
  std::string message;
};

struct EnvironmentRelation {
  std::string id;
  std::string component_id;
  std::string source_resource_id;
  std::string source_type;
  std::string source_name;
  std::string directive;
  std::string target_resource_id;
  std::string target_component_id;
  std::string target_type;
  std::string target_name;
};

struct LoadedComponent {
  std::string id;
  ComponentKind kind{};
  std::string component_id;
  std::string display_name;
  std::string name;
  std::unique_ptr<ConfigurationParser> parser;
  std::vector<EnvironmentResource> resources;

  ~LoadedComponent();
};

class Environment {
 public:
  ~Environment();

  const std::string& id() const { return id_; }
  const std::string& config_path() const { return config_path_; }
  std::vector<std::unique_ptr<LoadedComponent>>& components()
  {
    return components_;
  }
  const std::vector<std::unique_ptr<LoadedComponent>>& components() const
  {
    return components_;
  }
  std::vector<EnvironmentIssue>& issues() { return issues_; }
  const std::vector<EnvironmentIssue>& issues() const { return issues_; }
  std::vector<EnvironmentRelation>& relations() { return relations_; }
  const std::vector<EnvironmentRelation>& relations() const
  {
    return relations_;
  }

 private:
  friend std::unique_ptr<Environment> LoadEnvironment(const char* config_path);

  std::string id_;
  std::string config_path_;
  std::vector<std::unique_ptr<LoadedComponent>> components_;
  std::vector<EnvironmentIssue> issues_;
  std::vector<EnvironmentRelation> relations_;
};

std::unique_ptr<Environment> LoadEnvironment(const char* config_path);
const EnvironmentResource* FindResource(const Environment& environment,
                                        std::string_view component_id,
                                        std::string_view type_name,
                                        std::string_view resource_name);
std::string FormatComponentKind(ComponentKind component);

}  // namespace bconfig

#endif  // BAREOS_BCONFIG_BCONFIG_H_
