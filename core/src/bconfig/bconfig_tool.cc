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

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if HAVE_WIN32
#  include <io.h>
#  define BCONFIG_ISATTY _isatty
#  define BCONFIG_FILENO _fileno
#else
#  include <unistd.h>
#  define BCONFIG_ISATTY isatty
#  define BCONFIG_FILENO fileno
#endif

#include <readline/history.h>
#include <readline/readline.h>

#include "lib/cli.h"
#include "lib/bareos_resource.h"
#include "lib/output_formatter.h"
#include "lib/output_formatter_resource.h"

namespace {

enum class ShellEntryKind
{
  kInvalid,
  kRootDirectory,
  kComponentDirectory,
  kTypeDirectory,
  kResourceDirectory,
  kComponentSummaryFile,
  kResourceConfigFile,
  kResourceRelationsFile
};

struct ShellEntry {
  ShellEntryKind kind{ShellEntryKind::kInvalid};
  const bconfig::LoadedComponent* component{nullptr};
  const bconfig::EnvironmentResource* resource{nullptr};
  std::string type_name;
  std::string error;
};

std::vector<std::string> ParseCommandWords(const std::string& line);

ShellEntry MakeShellEntry(ShellEntryKind kind,
                          const bconfig::LoadedComponent* component = nullptr,
                          const bconfig::EnvironmentResource* resource
                          = nullptr,
                          std::string type_name = {},
                          std::string error = {})
{
  return ShellEntry{kind, component, resource, std::move(type_name),
                    std::move(error)};
}

const bconfig::Environment* shell_completion_environment = nullptr;
const std::vector<std::string>* shell_completion_path = nullptr;
std::vector<std::string> shell_completion_matches;

char* DuplicateReadlineString(const std::string& value)
{
  auto* copy = static_cast<char*>(std::malloc(value.size() + 1));
  if (!copy) { return nullptr; }
  std::memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

char* ShellCompletionGenerator(const char*, int state)
{
  static size_t index = 0;
  if (state == 0) { index = 0; }
  if (index >= shell_completion_matches.size()) { return nullptr; }
  return DuplicateReadlineString(shell_completion_matches[index++]);
}

char** ShellReadlineCompletion(const char* text, int start, int)
{
  if (!shell_completion_environment || !shell_completion_path) {
    return nullptr;
  }

  const std::string line = rl_line_buffer ? rl_line_buffer : "";
  const auto words = ParseCommandWords(line.substr(0, start));
  if (words.size() != 1 || words[0] != "cd") { return nullptr; }

  rl_attempted_completion_over = 1;
  shell_completion_matches = bconfig::CompleteShellPath(
      *shell_completion_environment, *shell_completion_path, text, true);
  if (shell_completion_matches.empty()) { return nullptr; }

  rl_completion_append_character = '\0';
  rl_completion_suppress_append = 1;
  return rl_completion_matches(text, ShellCompletionGenerator);
}

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
  std::cout << "Relations: " << environment.relations().size() << "\n";
  std::cout << "Issues: " << environment.issues().size() << "\n";
  std::cout << "Components:\n";

  for (const auto& component : environment.components()) {
    std::map<std::string, size_t> counts;
    for (const auto& resource : component->resources) {
      counts[resource.type]++;
    }

    std::cout << "  " << component->component_id;
    if (!component->name.empty()) { std::cout << " name=" << component->name; }
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

int RunInspectRelations(const bconfig::Environment& environment)
{
  std::cout << "Id | Component | Source Type | Source Name | Directive | "
               "Target Type | "
               "Target Name\n";
  for (const auto& relation : environment.relations()) {
    std::cout << relation.id << " | " << relation.component_id << " | "
              << relation.source_type << " | " << relation.source_name << " | "
              << relation.directive << " | " << relation.target_type << " | "
              << relation.target_name << "\n";
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

bool AppendToString(void* ctx, const char* fmt, ...)
{
  auto* output = static_cast<std::string*>(ctx);

  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  int size = vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (size < 0) {
    va_end(args);
    return false;
  }

  std::string buffer(size, '\0');
  vsnprintf(buffer.data(), buffer.size() + 1, fmt, args);
  va_end(args);

  output->append(buffer);
  return true;
}

const bconfig::LoadedComponent* FindComponent(
    const bconfig::Environment& environment,
    std::string_view component_id)
{
  for (const auto& component : environment.components()) {
    if (component->component_id == component_id) { return component.get(); }
  }

  return nullptr;
}

const bconfig::EnvironmentResource* FindComponentResource(
    const bconfig::LoadedComponent& component,
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

std::vector<std::string> ParseCommandWords(const std::string& line)
{
  std::vector<std::string> words;
  std::string current;
  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') {
      in_quotes = !in_quotes;
      continue;
    }

    if (!in_quotes && std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }

    current.push_back(ch);
  }

  if (!current.empty()) { words.push_back(current); }
  return words;
}

std::vector<std::string> NormalizePath(const std::vector<std::string>& current,
                                       const std::string& raw_path)
{
  std::vector<std::string> segments;
  if (!raw_path.empty() && raw_path[0] != '/') { segments = current; }

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

std::string FormatPath(const std::vector<std::string>& segments)
{
  if (segments.empty()) { return "/"; }

  std::ostringstream output;
  for (const auto& segment : segments) { output << '/' << segment; }
  return output.str();
}

ShellEntry ResolveShellEntry(const bconfig::Environment& environment,
                             const std::vector<std::string>& segments)
{
  if (segments.empty()) {
    return MakeShellEntry(ShellEntryKind::kRootDirectory);
  }
  if (segments.size() > 4) {
    return MakeShellEntry(ShellEntryKind::kInvalid, nullptr, nullptr, {},
                          "path depth exceeds shell view");
  }

  const auto* component = FindComponent(environment, segments[0]);
  if (!component) {
    return MakeShellEntry(ShellEntryKind::kInvalid, nullptr, nullptr, {},
                          "component does not exist");
  }
  if (segments.size() == 1) {
    return MakeShellEntry(ShellEntryKind::kComponentDirectory, component);
  }

  if (segments[1] == "summary") {
    if (segments.size() == 2) {
      return MakeShellEntry(ShellEntryKind::kComponentSummaryFile, component);
    }
    return MakeShellEntry(ShellEntryKind::kInvalid, nullptr, nullptr, {},
                          "summary is a file");
  }

  const std::string& type_name = segments[1];
  bool has_type = std::any_of(
      component->resources.begin(), component->resources.end(),
      [&](const auto& resource) { return resource.type == type_name; });
  if (!has_type) {
    return MakeShellEntry(ShellEntryKind::kInvalid, nullptr, nullptr, {},
                          "resource type does not exist");
  }
  if (segments.size() == 2) {
    return MakeShellEntry(ShellEntryKind::kTypeDirectory, component, nullptr,
                          type_name);
  }

  const auto* resource
      = FindComponentResource(*component, type_name, segments[2]);
  if (!resource) {
    return MakeShellEntry(ShellEntryKind::kInvalid, nullptr, nullptr, {},
                          "resource does not exist");
  }
  if (segments.size() == 3) {
    return MakeShellEntry(ShellEntryKind::kResourceDirectory, component,
                          resource, type_name);
  }

  if (segments[3] == "config") {
    return MakeShellEntry(ShellEntryKind::kResourceConfigFile, component,
                          resource, type_name);
  }
  if (segments[3] == "relations") {
    return MakeShellEntry(ShellEntryKind::kResourceRelationsFile, component,
                          resource, type_name);
  }

  return MakeShellEntry(ShellEntryKind::kInvalid, nullptr, nullptr, {},
                        "resource file does not exist");
}

bool IsDirectory(ShellEntryKind kind)
{
  return kind == ShellEntryKind::kRootDirectory
         || kind == ShellEntryKind::kComponentDirectory
         || kind == ShellEntryKind::kTypeDirectory
         || kind == ShellEntryKind::kResourceDirectory;
}

std::string RenderComponentSummary(const bconfig::LoadedComponent& component)
{
  std::map<std::string, size_t> counts;
  for (const auto& resource : component.resources) { counts[resource.type]++; }

  std::ostringstream output;
  output << "Component: " << component.component_id << '\n';
  if (!component.name.empty()) { output << "Name: " << component.name << '\n'; }
  output << "Resources: " << component.resources.size() << '\n';
  for (const auto& [type, count] : counts) {
    output << type << ": " << count << '\n';
  }

  return output.str();
}

std::string RenderResourceConfig(const bconfig::LoadedComponent& component,
                                 const bconfig::EnvironmentResource& resource)
{
  std::string output;
  OutputFormatter formatter(AppendToString, &output, nullptr, nullptr);
  OutputFormatterResource send(&formatter);
  auto* mutable_resource = const_cast<BareosResource*>(resource.resource);
  mutable_resource->PrintConfig(send, *component.parser);
  return output;
}

std::string RenderResourceRelations(
    const bconfig::Environment& environment,
    const bconfig::EnvironmentResource& resource)
{
  std::ostringstream output;
  output << "Outgoing:\n";
  bool found = false;
  for (const auto& relation : environment.relations()) {
    if (relation.source_resource_id != resource.id) { continue; }
    found = true;
    output << "  " << relation.directive << " -> "
           << relation.target_component_id << ':' << relation.target_type << '/'
           << relation.target_name << '\n';
  }
  if (!found) { output << "  (none)\n"; }

  output << "Incoming:\n";
  found = false;
  for (const auto& relation : environment.relations()) {
    if (relation.target_resource_id != resource.id) { continue; }
    found = true;
    output << "  " << relation.component_id << ':' << relation.source_type
           << '/' << relation.source_name << " -> " << relation.directive
           << '\n';
  }
  if (!found) { output << "  (none)\n"; }

  return output.str();
}

bool ReadShellLine(const std::string& prompt,
                   bool interactive,
                   std::string& line)
{
  if (!interactive) { return static_cast<bool>(std::getline(std::cin, line)); }

  char* raw_line = readline(prompt.c_str());
  if (!raw_line) { return false; }
  line = raw_line;
  if (!line.empty()) {
    auto* last_history_item
        = history_length > 0 ? history_get(history_length) : nullptr;
    if (!last_history_item || line != last_history_item->line) {
      add_history(raw_line);
    }
  }
  std::free(raw_line);
  return true;
}

int RunShell(const bconfig::Environment& environment)
{
  std::vector<std::string> current_path;
  const bool interactive = BCONFIG_ISATTY(BCONFIG_FILENO(stdin));

  if (interactive) {
    std::cout << "Read-only bconfig shell. Commands: pwd, ls, cd, cat, help, "
                 "exit\n";
    using_history();
    shell_completion_environment = &environment;
    shell_completion_path = &current_path;
    rl_attempted_completion_function = ShellReadlineCompletion;
    rl_filename_completion_desired = 0;
  }

  std::string line;
  while (true) {
    const std::string prompt = "bconfig:" + FormatPath(current_path) + "$ ";
    if (!ReadShellLine(prompt, interactive, line)) { return 0; }

    const auto words = ParseCommandWords(line);
    if (words.empty()) { continue; }

    const auto& command = words[0];
    if (command == "exit" || command == "quit") { return 0; }
    if (command == "help") {
      std::cout << "pwd\nls [path]\ncd [path]\ncat <path>\nhelp\nexit\n";
      continue;
    }
    if (command == "pwd") {
      std::cout << FormatPath(current_path) << '\n';
      continue;
    }
    if (command == "cd") {
      const std::string target = words.size() > 1 ? words[1] : "/";
      const auto segments = NormalizePath(current_path, target);
      const auto entry = ResolveShellEntry(environment, segments);
      if (!IsDirectory(entry.kind)) {
        std::cout << "cd: " << entry.error << '\n';
        continue;
      }
      current_path = segments;
      continue;
    }
    if (command == "ls") {
      const std::string target = words.size() > 1 ? words[1] : ".";
      const auto segments = NormalizePath(current_path, target);
      const auto entry = ResolveShellEntry(environment, segments);
      if (!IsDirectory(entry.kind)) {
        std::cout << "ls: " << entry.error << '\n';
        continue;
      }

      std::set<std::string> entries;
      switch (entry.kind) {
        case ShellEntryKind::kRootDirectory:
          for (const auto& component : environment.components()) {
            entries.insert(component->component_id + "/");
          }
          break;
        case ShellEntryKind::kComponentDirectory:
          entries.insert("summary");
          for (const auto& resource : entry.component->resources) {
            entries.insert(resource.type + "/");
          }
          break;
        case ShellEntryKind::kTypeDirectory:
          for (const auto& resource : entry.component->resources) {
            if (resource.type == entry.type_name) {
              entries.insert(resource.name + "/");
            }
          }
          break;
        case ShellEntryKind::kResourceDirectory:
          entries.insert("config");
          entries.insert("relations");
          break;
        default:
          break;
      }

      for (const auto& item : entries) { std::cout << item << '\n'; }
      continue;
    }
    if (command == "cat") {
      if (words.size() < 2) {
        std::cout << "cat: missing path\n";
        continue;
      }

      const auto segments = NormalizePath(current_path, words[1]);
      const auto entry = ResolveShellEntry(environment, segments);
      if (entry.kind == ShellEntryKind::kComponentSummaryFile) {
        std::cout << RenderComponentSummary(*entry.component);
        continue;
      }
      if (entry.kind == ShellEntryKind::kResourceConfigFile) {
        std::cout << RenderResourceConfig(*entry.component, *entry.resource);
        continue;
      }
      if (entry.kind == ShellEntryKind::kResourceRelationsFile) {
        std::cout << RenderResourceRelations(environment, *entry.resource);
        continue;
      }

      if (entry.kind == ShellEntryKind::kInvalid) {
        std::cout << "cat: " << entry.error << '\n';
      } else {
        std::cout << "cat: path is a directory\n";
      }
      continue;
    }

    std::cout << "unknown command: " << command << '\n';
  }
}

}  // namespace

int main(int argc, char** argv)
{
  CLI::App app;
  InitCLIApp(app, "Inspect the whole Bareos environment");

  std::string config_path;

  auto* inspect = app.add_subcommand("inspect", "Inspect environment state");
  inspect->require_subcommand(1);
  auto* shell = app.add_subcommand("shell", "Browse environment as shell");

  auto add_common_options = [&](CLI::App* command) {
    command->add_option("--config", config_path,
                        "Config file or config root for the whole environment");
  };

  auto* summary
      = inspect->add_subcommand("summary", "Show environment summary");
  add_common_options(summary);

  auto* resources
      = inspect->add_subcommand("resources", "List environment resources");
  add_common_options(resources);

  auto* relations = inspect->add_subcommand(
      "relations", "List resolved environment relations");
  add_common_options(relations);
  add_common_options(shell);

  ParseBareosApp(app, argc, argv);

  const char* config_arg = config_path.empty() ? nullptr : config_path.c_str();
  auto environment = bconfig::LoadEnvironment(config_arg);

  if (*summary) { return RunInspectSummary(*environment); }
  if (*resources) { return RunInspectResources(*environment); }
  if (*relations) { return RunInspectRelations(*environment); }
  if (*shell) { return RunShell(*environment); }

  return 1;
}
