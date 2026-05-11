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

#include <iostream>
#include <map>

#include "lib/cli.h"

namespace {

int RunInspectSummary(const bconfig::Environment& environment)
{
  std::cout << "Environment: " << environment.id() << "\n";
  std::cout << "Config base: " << environment.config_path() << "\n";
  std::cout << "Loaded components: " << environment.components().size() << "\n";

  size_t resource_count = 0;
  for (const auto& component : environment.components()) {
    resource_count += component->resources.size();
  }

  std::cout << "Resources: " << resource_count << "\n";
  std::cout << "Issues: " << environment.issues().size() << "\n";
  std::cout << "Components:\n";

  for (const auto& component : environment.components()) {
    std::map<std::string, size_t> counts;
    for (const auto& resource : component->resources) {
      counts[resource.type]++;
    }

    std::cout << "  " << component->component_id;
    if (!component->name.empty()) {
      std::cout << " name=" << component->name;
    }
    std::cout << ": " << component->resources.size() << " resources\n";
    for (const auto& [type, count] : counts) {
      std::cout << "    " << type << ": " << count << "\n";
    }
  }

  if (!environment.issues().empty()) {
    std::cout << "Environment issues:\n";
    for (const auto& issue : environment.issues()) {
      std::cout << "  [" << issue.component_id << "] " << issue.message << "\n";
    }
  }

  return 0;
}

int RunInspectResources(const bconfig::Environment& environment)
{
  std::cout << "Id | Component | Type | Name\n";
  for (const auto& component : environment.components()) {
    for (const auto& resource : component->resources) {
      std::cout << resource.id << " | " << component->component_id << " | "
                << resource.type << " | " << resource.name << "\n";
    }
  }

  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  CLI::App app;
  InitCLIApp(app, "Inspect the whole Bareos environment");

  std::string config_path;

  auto* inspect = app.add_subcommand("inspect", "Inspect environment state");
  inspect->require_subcommand(1);

  auto add_common_options = [&](CLI::App* command) {
    command->add_option("--config", config_path,
                        "Config file or config root for the whole environment");
  };

  auto* summary = inspect->add_subcommand("summary", "Show environment summary");
  add_common_options(summary);

  auto* resources
      = inspect->add_subcommand("resources", "List environment resources");
  add_common_options(resources);

  ParseBareosApp(app, argc, argv);

  const char* config_arg = config_path.empty() ? nullptr : config_path.c_str();
  auto environment = bconfig::LoadEnvironment(config_arg);

  if (*summary) { return RunInspectSummary(*environment); }
  if (*resources) { return RunInspectResources(*environment); }

  return 1;
}
