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

#include <filesystem>
#include <memory>
#include <set>
#include <unordered_map>

#include "console/console_conf.h"
#include "console/console_globals.h"
#include "dird/dird_conf.h"
#include "dird/dird_globals.h"
#include "lib/alist.h"
#include "filed/filed_conf.h"
#include "filed/filed_globals.h"
#include "lib/bareos_resource.h"
#include "lib/parse_conf.h"
#include "lib/resource_item.h"
#include "qt-tray-monitor/tray_conf.h"
#include "stored/stored_conf.h"
#include "stored/stored_globals.h"

ConfigurationParser* my_config = nullptr;

namespace bconfig {

namespace {

namespace fs = std::filesystem;

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
  const char* default_config_filename;
  const char* include_dir_name;
  bool optional;
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

void PrepareConsole()
{
  InitRuntime();
  console::my_config = nullptr;
  console::me = nullptr;
  console::console_resource = nullptr;
  console::director_resource = nullptr;
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

void BindConsoleParser(ConfigurationParser* parser)
{
  console::my_config = parser;
}

void PrepareTrayMonitor()
{
  InitRuntime();
  ::my_config = nullptr;
}

void BindTrayMonitorParser(ConfigurationParser* parser)
{
  ::my_config = parser;
}

const ComponentDefinition kComponents[] = {
    {ComponentKind::kDirector, "director", "Bareos Director", "Director",
     directordaemon::default_config_filename.c_str(), "bareos-dir.d", false,
     directordaemon::InitDirConfig, PrepareDirector, BindDirectorParser},
    {ComponentKind::kStorageDaemon, "storage-daemon", "Bareos Storage Daemon",
     "Storage", storagedaemon::default_config_filename.c_str(), "bareos-sd.d",
     false, storagedaemon::InitSdConfig, PrepareStorage, BindStorageParser},
    {ComponentKind::kFileDaemon, "file-daemon", "Bareos File Daemon", "Client",
     filedaemon::default_config_filename.c_str(), "bareos-fd.d", false,
     filedaemon::InitFdConfig, PrepareFileDaemon, BindFileDaemonParser},
    {ComponentKind::kConsole, "console", "Bareos Console", "Console",
     console::default_config_filename.c_str(), "bconsole.d", true,
     console::InitConsConfig, PrepareConsole, BindConsoleParser},
    {ComponentKind::kTrayMonitor, "tray-monitor", "Bareos Tray Monitor",
     "Monitor", "tray-monitor.conf", "tray-monitor.d", true, InitTmonConfig,
     PrepareTrayMonitor, BindTrayMonitorParser},
};

bool ComponentConfigExists(const ComponentDefinition& definition,
                           const char* config_path)
{
  const fs::path base_path
      = config_path ? fs::path(config_path)
                    : fs::path(ConfigurationParser::GetDefaultConfigDir());

  if (!fs::exists(base_path)) { return false; }
  if (!fs::is_directory(base_path)) { return true; }

  const auto config_file = base_path / definition.default_config_filename;
  const auto include_dir = base_path / definition.include_dir_name;
  return fs::exists(config_file) || fs::exists(include_dir);
}

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

template <typename T>
const T* MemberPointer(const BareosResource* resource, const ResourceItem& item)
{
  auto* base = reinterpret_cast<const char*>(resource);
  return reinterpret_cast<const T*>(base + item.offset);
}

const EnvironmentResource* FindComponentResource(const Environment& environment,
                                                 ComponentKind component_kind,
                                                 std::string_view type_name,
                                                 std::string_view resource_name)
{
  for (const auto& component : environment.components()) {
    if (component->kind != component_kind) { continue; }
    for (const auto& resource : component->resources) {
      if (resource.type == type_name && resource.name == resource_name) {
        return &resource;
      }
    }
  }

  return nullptr;
}

const LoadedComponent* FindShellComponent(const Environment& environment,
                                          std::string_view component_id)
{
  for (const auto& component : environment.components()) {
    if (component->component_id == component_id) { return component.get(); }
  }

  return nullptr;
}

const EnvironmentResource* FindShellResource(const LoadedComponent& component,
                                             std::string_view type_name,
                                             std::string_view resource_name)
{
  for (const auto& resource : component.resources) {
    if (resource.type == type_name && resource.name == resource_name) {
      return &resource;
    }
  }

  return nullptr;
}

std::vector<std::string> NormalizeShellPath(
    const std::vector<std::string>& current,
    std::string_view raw_path)
{
  std::vector<std::string> segments;
  if (raw_path.empty() || raw_path.front() != '/') { segments = current; }

  std::string current_segment;
  auto flush_segment = [&]() {
    if (current_segment.empty() || current_segment == ".") {
      current_segment.clear();
      return;
    }
    if (current_segment == "..") {
      if (!segments.empty()) { segments.pop_back(); }
      current_segment.clear();
      return;
    }
    segments.push_back(current_segment);
    current_segment.clear();
  };

  for (char ch : raw_path) {
    if (ch == '/') {
      flush_segment();
      continue;
    }
    current_segment.push_back(ch);
  }
  flush_segment();

  return segments;
}

bool CollectShellDirectoryEntries(const Environment& environment,
                                  const std::vector<std::string>& segments,
                                  bool directories_only,
                                  std::set<std::string>& entries)
{
  if (segments.empty()) {
    for (const auto& component : environment.components()) {
      entries.insert(component->component_id + "/");
    }
    return true;
  }
  if (segments.size() > 3) { return false; }

  const auto* component = FindShellComponent(environment, segments[0]);
  if (!component) { return false; }
  if (segments.size() == 1) {
    if (!directories_only) { entries.insert("summary"); }
    for (const auto& resource : component->resources) {
      entries.insert(resource.type + "/");
    }
    return true;
  }

  if (segments[1] == "summary") { return false; }

  const std::string& type_name = segments[1];
  bool has_type = std::any_of(
      component->resources.begin(), component->resources.end(),
      [&](const auto& resource) { return resource.type == type_name; });
  if (!has_type) { return false; }
  if (segments.size() == 2) {
    for (const auto& resource : component->resources) {
      if (resource.type == type_name) { entries.insert(resource.name + "/"); }
    }
    return true;
  }

  const auto* resource = FindShellResource(*component, type_name, segments[2]);
  if (!resource) { return false; }
  (void)resource;
  if (!directories_only) {
    entries.insert("config");
    entries.insert("relations");
  }
  return true;
}

bool PasswordsMatch(const s_password& left, const s_password& right)
{
  if (left.encoding != right.encoding) { return false; }
  if (!left.value || !right.value) { return false; }
  return std::string_view(left.value) == right.value;
}

void AppendSingleRelation(
    const LoadedComponent& component,
    const EnvironmentResource& source_resource,
    std::string_view directive_name,
    const BareosResource* target_resource,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup,
    IdGenerator& relation_ids,
    std::vector<EnvironmentRelation>& relations)
{
  if (!target_resource) { return; }

  auto target = resource_lookup.find(target_resource);
  if (target == resource_lookup.end()) { return; }

  relations.push_back(EnvironmentRelation{
      relation_ids.Next(),
      component.component_id,
      source_resource.id,
      source_resource.type,
      source_resource.name,
      std::string(directive_name),
      target->second->id,
      target->second->component_id,
      target->second->type,
      target->second->name,
  });
}

void CollectEnvironmentRelations(
    Environment& environment,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup)
{
  IdGenerator relation_ids("rel");

  for (const auto& component : environment.components()) {
    for (const auto& resource : component->resources) {
      const auto* table
          = component->parser->GetResourceTable(resource.type.c_str());
      if (!table || !table->items) { continue; }

      for (int index = 0; table->items[index].name; ++index) {
        const auto& item = table->items[index];
        if (item.type == CFG_TYPE_RES) {
          auto target_resource
              = *MemberPointer<BareosResource*>(resource.resource, item);
          AppendSingleRelation(*component, resource, item.name, target_resource,
                               resource_lookup, relation_ids,
                               environment.relations());
        } else if (item.type == CFG_TYPE_ALIST_RES) {
          auto list = *MemberPointer<alist<BareosResource*>*>(resource.resource,
                                                              item);
          int list_index = 0;
          BareosResource* target_resource = nullptr;
          foreach_alist_index (list_index, target_resource, list) {
            AppendSingleRelation(*component, resource, item.name,
                                 target_resource, resource_lookup, relation_ids,
                                 environment.relations());
          }
        }
      }
    }
  }
}

void CollectScheduleRelations(
    Environment& environment,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup)
{
  IdGenerator relation_ids("rel");
  for (size_t i = 0; i < environment.relations().size(); ++i) {
    relation_ids.Next();
  }

  for (const auto& component : environment.components()) {
    if (component->kind != ComponentKind::kDirector) { continue; }

    for (const auto& resource : component->resources) {
      if (resource.type != "Schedule" || !resource.resource) { continue; }

      auto* schedule = dynamic_cast<const directordaemon::ScheduleResource*>(
          resource.resource);
      if (!schedule) { continue; }

      for (auto* run = schedule->run; run; run = run->next) {
        AppendSingleRelation(*component, resource, {"Run/Pool"}, run->pool,
                             resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/FullPool"},
                             run->full_pool, resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/VFullPool"},
                             run->vfull_pool, resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/IncrementalPool"},
                             run->inc_pool, resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/DifferentialPool"},
                             run->diff_pool, resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/NextPool"},
                             run->next_pool, resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/Storage"},
                             run->storage, resource_lookup, relation_ids,
                             environment.relations());
        AppendSingleRelation(*component, resource, {"Run/Messages"}, run->msgs,
                             resource_lookup, relation_ids,
                             environment.relations());
      }
    }
  }
}

void CollectDirectorStorageRelations(
    Environment& environment,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup)
{
  IdGenerator relation_ids("rel");
  for (size_t i = 0; i < environment.relations().size(); ++i) {
    relation_ids.Next();
  }

  for (const auto& component : environment.components()) {
    if (component->kind != ComponentKind::kDirector) { continue; }

    for (const auto& resource : component->resources) {
      if (resource.type != "Storage" || !resource.resource) { continue; }

      auto* storage = dynamic_cast<const directordaemon::StorageResource*>(
          resource.resource);
      if (!storage) { continue; }

      for (const auto& device : storage->devices) {
        const auto* target = FindComponentResource(
            environment, ComponentKind::kStorageDaemon, "Device", device.name);
        if (!target) {
          target = FindComponentResource(environment,
                                         ComponentKind::kStorageDaemon,
                                         "Autochanger", device.name);
        }
        if (!target) { continue; }

        AppendSingleRelation(*component, resource, {"Device"}, target->resource,
                             resource_lookup, relation_ids,
                             environment.relations());
      }
    }
  }
}

void CollectDirectorAuthenticationRelations(
    Environment& environment,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup)
{
  IdGenerator relation_ids("rel");
  for (size_t i = 0; i < environment.relations().size(); ++i) {
    relation_ids.Next();
  }

  for (const auto& component : environment.components()) {
    if (component->kind != ComponentKind::kDirector
        || component->name.empty()) {
      continue;
    }

    const auto* filed_director = FindComponentResource(
        environment, ComponentKind::kFileDaemon, "Director", component->name);
    const auto* stored_director
        = FindComponentResource(environment, ComponentKind::kStorageDaemon,
                                "Director", component->name);

    for (const auto& resource : component->resources) {
      if (resource.type == "Client" && resource.resource && filed_director) {
        auto* client = dynamic_cast<const directordaemon::ClientResource*>(
            resource.resource);
        auto* remote_director
            = dynamic_cast<const filedaemon::DirectorResource*>(
                filed_director->resource);
        if (client && remote_director
            && PasswordsMatch(client->password_, remote_director->password_)) {
          AppendSingleRelation(*component, resource,
                               {"Authentication/Password"},
                               filed_director->resource, resource_lookup,
                               relation_ids, environment.relations());
        }
      } else if (resource.type == "Storage" && resource.resource
                 && stored_director) {
        auto* storage = dynamic_cast<const directordaemon::StorageResource*>(
            resource.resource);
        auto* remote_director
            = dynamic_cast<const storagedaemon::DirectorResource*>(
                stored_director->resource);
        if (storage && remote_director
            && PasswordsMatch(storage->password_, remote_director->password_)) {
          AppendSingleRelation(*component, resource,
                               {"Authentication/Password"},
                               stored_director->resource, resource_lookup,
                               relation_ids, environment.relations());
        }
      }
    }
  }
}

void CollectConsoleAuthenticationRelations(
    Environment& environment,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup)
{
  IdGenerator relation_ids("rel");
  for (size_t i = 0; i < environment.relations().size(); ++i) {
    relation_ids.Next();
  }

  for (const auto& component : environment.components()) {
    if (component->kind != ComponentKind::kConsole) { continue; }

    for (const auto& resource : component->resources) {
      if (resource.type == "Console" && resource.resource) {
        auto* console_resource
            = dynamic_cast<const console::ConsoleResource*>(resource.resource);
        if (!console_resource) { continue; }

        const auto* director_console = FindComponentResource(
            environment, ComponentKind::kDirector, "Console", resource.name);
        if (!director_console) { continue; }

        auto* remote_console
            = dynamic_cast<const directordaemon::ConsoleResource*>(
                director_console->resource);
        if (!remote_console) { continue; }

        if (!PasswordsMatch(console_resource->password_,
                            remote_console->password_)) {
          continue;
        }

        AppendSingleRelation(*component, resource, {"Authentication/Password"},
                             director_console->resource, resource_lookup,
                             relation_ids, environment.relations());
      } else if (resource.type == "Director" && resource.resource) {
        auto* console_director
            = dynamic_cast<const console::DirectorResource*>(resource.resource);
        if (!console_director) { continue; }

        const auto* director_resource = FindComponentResource(
            environment, ComponentKind::kDirector, "Director", resource.name);
        if (!director_resource) { continue; }

        auto* remote_director
            = dynamic_cast<const directordaemon::DirectorResource*>(
                director_resource->resource);
        if (!remote_director) { continue; }

        if (!PasswordsMatch(console_director->password_,
                            remote_director->password_)) {
          continue;
        }

        AppendSingleRelation(*component, resource, {"Authentication/Password"},
                             director_resource->resource, resource_lookup,
                             relation_ids, environment.relations());
      }
    }
  }
}

void CollectTrayMonitorAuthenticationRelations(
    Environment& environment,
    const std::unordered_map<const BareosResource*, const EnvironmentResource*>&
        resource_lookup)
{
  IdGenerator relation_ids("rel");
  for (size_t i = 0; i < environment.relations().size(); ++i) {
    relation_ids.Next();
  }

  for (const auto& component : environment.components()) {
    if (component->kind != ComponentKind::kTrayMonitor
        || component->name.empty()) {
      continue;
    }

    const auto* director_console = FindComponentResource(
        environment, ComponentKind::kDirector, "Console", component->name);
    const auto* filed_director = FindComponentResource(
        environment, ComponentKind::kFileDaemon, "Director", component->name);
    const auto* stored_director
        = FindComponentResource(environment, ComponentKind::kStorageDaemon,
                                "Director", component->name);

    const MonitorResource* monitor = nullptr;
    for (const auto& resource : component->resources) {
      if (resource.type == "Monitor" && resource.resource) {
        monitor = dynamic_cast<const MonitorResource*>(resource.resource);
        if (monitor) { break; }
      }
    }

    if (!monitor) { continue; }

    for (const auto& resource : component->resources) {
      if (resource.type == "Monitor" && resource.resource && director_console) {
        auto* remote_console
            = dynamic_cast<const directordaemon::ConsoleResource*>(
                director_console->resource);
        if (!remote_console) { continue; }
        if (!PasswordsMatch(monitor->password, remote_console->password_)) {
          continue;
        }

        AppendSingleRelation(*component, resource, {"Authentication/Password"},
                             director_console->resource, resource_lookup,
                             relation_ids, environment.relations());
      } else if (resource.type == "Client" && resource.resource
                 && filed_director) {
        auto* client = dynamic_cast<const ClientResource*>(resource.resource);
        auto* remote_director
            = dynamic_cast<const filedaemon::DirectorResource*>(
                filed_director->resource);
        if (!client || !remote_director) { continue; }
        if (!PasswordsMatch(client->password, remote_director->password_)) {
          continue;
        }

        AppendSingleRelation(*component, resource, {"Authentication/Password"},
                             filed_director->resource, resource_lookup,
                             relation_ids, environment.relations());
      } else if (resource.type == "Storage" && resource.resource
                 && stored_director) {
        auto* storage = dynamic_cast<const StorageResource*>(resource.resource);
        auto* remote_director
            = dynamic_cast<const storagedaemon::DirectorResource*>(
                stored_director->resource);
        if (!storage || !remote_director) { continue; }
        if (!PasswordsMatch(storage->password, remote_director->password_)) {
          continue;
        }

        AppendSingleRelation(*component, resource, {"Authentication/Password"},
                             stored_director->resource, resource_lookup,
                             relation_ids, environment.relations());
      }
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
  std::unordered_map<const BareosResource*, const EnvironmentResource*>
      resource_lookup;

  for (const auto& definition : kComponents) {
    if (definition.optional
        && !ComponentConfigExists(definition, config_path)) {
      continue;
    }

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
    for (const auto& resource : component->resources) {
      resource_lookup.emplace(resource.resource, &resource);
    }
    environment->components_.push_back(std::move(component));
  }

  CollectEnvironmentRelations(*environment, resource_lookup);
  CollectScheduleRelations(*environment, resource_lookup);
  CollectDirectorStorageRelations(*environment, resource_lookup);
  CollectDirectorAuthenticationRelations(*environment, resource_lookup);
  CollectConsoleAuthenticationRelations(*environment, resource_lookup);
  CollectTrayMonitorAuthenticationRelations(*environment, resource_lookup);

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

std::vector<std::string> CompleteShellPath(
    const Environment& environment,
    const std::vector<std::string>& current,
    std::string_view raw_path,
    bool directories_only)
{
  const std::string raw(raw_path);
  std::string base_prefix;
  std::string fragment = raw;
  std::vector<std::string> parent_segments = current;

  const auto slash = raw.find_last_of('/');
  if (slash != std::string::npos) {
    base_prefix = raw.substr(0, slash + 1);
    fragment = raw.substr(slash + 1);
    const std::string parent_path = slash == 0 ? "/" : raw.substr(0, slash);
    parent_segments = NormalizeShellPath(current, parent_path);
  } else if (!raw.empty() && raw.front() == '/') {
    parent_segments = NormalizeShellPath(current, "/");
  }

  std::set<std::string> entries;
  if (!CollectShellDirectoryEntries(environment, parent_segments,
                                    directories_only, entries)) {
    return {};
  }

  std::vector<std::string> matches;
  for (const auto& entry : entries) {
    if (entry.rfind(fragment, 0) != 0) { continue; }
    matches.push_back(base_prefix + entry);
  }
  return matches;
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
    case ComponentKind::kConsole:
      return "console";
    case ComponentKind::kTrayMonitor:
      return "tray-monitor";
  }

  return "unknown";
}

}  // namespace bconfig
