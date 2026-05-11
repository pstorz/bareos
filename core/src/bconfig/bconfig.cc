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

#include "bconfig/bconfig.h"

#include <memory>

#include "console/console_globals.h"
#include "dird/dird_conf.h"
#include "dird/dird_globals.h"
#include "filed/filed_conf.h"
#include "filed/filed_globals.h"
#include "lib/bareos_resource.h"
#include "lib/parse_conf.h"
#include "stored/stored_conf.h"
#include "stored/stored_globals.h"

namespace bconfig {

namespace {

class IdGenerator {
 public:
  explicit IdGenerator(const char* prefix) : prefix_(prefix) {}

  std::string Next()
  {
    ++counter_;
    char buffer[32];
    Bsnprintf(buffer, sizeof(buffer), "%s_%06zu", prefix_, counter_);
    return buffer;
  }

 private:
  const char* prefix_;
  size_t counter_{0};
};

struct ComponentDefinition {
  ComponentKind kind;
  const char* id;
  const char* display_name;
  const char* primary_resource_type;
  ConfigurationParser* (*init)(const char* config_path, int exit_code);
  void (*prepare)();
  void (*bind_parser)(ConfigurationParser*);
};

void InitRuntime()
{
  OSDependentInit();
#if HAVE_WIN32
  WSA_Init();
#endif
}

void PrepareDirector()
{
  InitRuntime();
  directordaemon::my_config = nullptr;
  directordaemon::me = nullptr;
}

void PrepareStorage()
{
  InitRuntime();
  storagedaemon::my_config = nullptr;
  storagedaemon::me = nullptr;
}

void PrepareFileDaemon()
{
  InitRuntime();
  filedaemon::my_config = nullptr;
  filedaemon::me = nullptr;
}

void BindDirectorParser(ConfigurationParser* parser)
{
  directordaemon::my_config = parser;
}

void BindStorageParser(ConfigurationParser* parser)
{
  storagedaemon::my_config = parser;
}

void BindFileDaemonParser(ConfigurationParser* parser)
{
  filedaemon::my_config = parser;
}

const ComponentDefinition kComponents[] = {
    {ComponentKind::kDirector, "director", "Bareos Director", "Director",
     directordaemon::InitDirConfig, PrepareDirector, BindDirectorParser},
    {ComponentKind::kStorageDaemon, "storage-daemon", "Bareos Storage Daemon",
     "Storage", storagedaemon::InitSdConfig, PrepareStorage, BindStorageParser},
    {ComponentKind::kFileDaemon, "file-daemon", "Bareos File Daemon", "Client",
     filedaemon::InitFdConfig, PrepareFileDaemon, BindFileDaemonParser},
};

void CollectComponentResources(LoadedComponent& component,
                               IdGenerator& resource_ids)
{
  for (int rcode = 0; rcode < component.parser->r_num_; ++rcode) {
    const auto* table = &component.parser->resource_definitions_[rcode];
    if (!table->name) { continue; }

    for (auto* resource = component.parser->GetNextRes(rcode, nullptr);
         resource; resource = component.parser->GetNextRes(rcode, resource)) {
      component.resources.push_back(EnvironmentResource{
          resource_ids.Next(),
          component.kind,
          component.component_id,
          component.parser->ResToStr(rcode) ? component.parser->ResToStr(rcode)
                                            : table->name,
          component.parser->ResGroupToStr(rcode)
              ? component.parser->ResGroupToStr(rcode)
              : "",
          GetResourceName(resource) ? GetResourceName(resource) : "",
          resource,
      });
    }
  }
}

}  // namespace

LoadedComponent::~LoadedComponent() = default;
Environment::~Environment() = default;

std::unique_ptr<Environment> LoadEnvironment(const char* config_path)
{
  auto environment = std::make_unique<Environment>();
  IdGenerator environment_ids("env");
  IdGenerator component_ids("cmp");
  IdGenerator resource_ids("res");

  environment->id_ = environment_ids.Next();
  environment->config_path_
      = config_path ? config_path : ConfigurationParser::GetDefaultConfigDir();

  for (const auto& definition : kComponents) {
    definition.prepare();

    auto parser = std::unique_ptr<ConfigurationParser>(
        definition.init(config_path, M_INFO));
    if (!parser) {
      environment->issues_.push_back(
          {definition.kind, definition.id, "parser initialization failed"});
      continue;
    }

    parser->SetInspectionMode(true);
    definition.bind_parser(parser.get());
    if (!parser->ParseConfig()) {
      environment->issues_.push_back({definition.kind, definition.id,
                                      "configuration could not be parsed"});
      continue;
    }

    parser->own_resource_ = parser->GetNextRes(parser->r_own_, nullptr);

    auto component = std::make_unique<LoadedComponent>();
    component->id = component_ids.Next();
    component->kind = definition.kind;
    component->component_id = definition.id;
    component->display_name = definition.display_name;
    component->name
        = parser->own_resource_ ? GetResourceName(parser->own_resource_) : "";
    component->parser = std::move(parser);
    CollectComponentResources(*component, resource_ids);
    if (component->name.empty()) {
      auto it = std::find_if(
          component->resources.begin(), component->resources.end(),
          [&](const EnvironmentResource& resource) {
            return resource.type == definition.primary_resource_type;
          });
      if (it != component->resources.end()) { component->name = it->name; }
    }
    environment->components_.push_back(std::move(component));
  }

  return environment;
}

const EnvironmentResource* FindResource(const Environment& environment,
                                        std::string_view component_id,
                                        std::string_view type_name,
                                        std::string_view resource_name)
{
  for (const auto& component : environment.components()) {
    if (component->component_id != component_id) { continue; }
    for (const auto& resource : component->resources) {
      if (resource.type == type_name && resource.name == resource_name) {
        return &resource;
      }
    }
  }

  return nullptr;
}

std::string FormatComponentKind(ComponentKind component)
{
  switch (component) {
    case ComponentKind::kDirector:
      return "director";
    case ComponentKind::kStorageDaemon:
      return "storage-daemon";
    case ComponentKind::kFileDaemon:
      return "file-daemon";
  }

  return "unknown";
}

}  // namespace bconfig
