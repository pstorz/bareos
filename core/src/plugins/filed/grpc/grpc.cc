/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#include <cstdio>
#include <thread>
#include "grpc_impl.h"

#include "include/bareos.h"
#include "filed/fd_plugins.h"

#include <fmt/format.h>

#include "bareos_api.h"

namespace {
bool next_section(std::string_view& input, std::string& output, char delimiter)
{
  if (input.size() == 0) { return false; }

  bool escaped = false;

  size_t read_bytes = 0;

  while (input.size() > read_bytes) {
    auto c = input[read_bytes++];

    if (escaped) {
      output += c;
      escaped = false;
    } else if (c == delimiter) {
      break;
    } else if (c == '\\') {
      escaped = true;
    } else {
      output += c;
    }
  }

  if (escaped) {
    DebugLog(
        100,
        FMT_STRING("trailing backslash in \"{}\" detected! Refusing to parse!"),
        input);
    return false;
  }

  // we only want to advance the string once we have made sure that the parsing
  // succeded.
  input.remove_prefix(read_bytes);
  return true;
}


struct plugin_ctx {
  bool re_setup(PluginContext* bareos_ctx, const void* data)
  {
    if (needs_setup()) { return setup(bareos_ctx, data); }

    if (!bareos_ctx || !data) { return false; }

    std::string_view options_string{(const char*)data};

    std::string new_plugin_name{};
    std::string inferior_name{};

    if (!next_section(options_string, new_plugin_name, ':')) {
      DebugLog(50, FMT_STRING("could not parse plugin name in {}"),
               options_string);
      return false;
    }

    if (new_plugin_name != std::string_view{"grpc"}) {
      DebugLog(50, FMT_STRING("wrong plugin name ({}) supplied"),
               new_plugin_name);
      return false;
    }


    if (!next_section(options_string, inferior_name, ':')) {
      DebugLog(50, FMT_STRING("could not parse name in {}"), options_string);
      return false;
    }

    DebugLog(100, FMT_STRING("found name = {}"), inferior_name);

    // TODO: we probably want to allow some options for the grpc plugin itself
    //       as well.  Maybe the separator could be :: ? I.e.
    //       grpc:opt=val:opt=val::child:childopt=val:childopt=val:...
    if (new_plugin_name != plugin_name) {
      DebugLog(50, FMT_STRING("not same name ({} != {}) supplied"), plugin_name,
               new_plugin_name);
      return false;
    }

    cmd = options_string;
    name = std::move(inferior_name);

    return true;
  }


  bool setup(PluginContext* bareos_ctx, const void* data)
  {
    if (!bareos_ctx || !data) { return false; }

    std::string_view options_string{(const char*)data};

    // we expect options_string to be a ':'-delimited list of kv pairs;
    // the first "pair" is just the name of the plugin that we are supposed
    // to load.

    if (!next_section(options_string, plugin_name, ':')) {
      DebugLog(50, FMT_STRING("could not parse plugin name in {}"),
               options_string);
      return false;
    }

    if (plugin_name != std::string_view{"grpc"}) {
      DebugLog(50, FMT_STRING("wrong plugin name ({}) supplied"), plugin_name);
      return false;
    }


    if (!next_section(options_string, name, ':')) {
      DebugLog(50, FMT_STRING("could not parse name in {}"), options_string);
      return false;
    }

    DebugLog(100, FMT_STRING("found name = {}"), name);

    // TODO: we probably want to allow some options for the grpc plugin itself
    //       as well.  Maybe the separator could be :: ? I.e.
    //       grpc:opt=val:opt=val::child:childopt=val:childopt=val:...
    cmd = options_string;
    // {
    //   std::string kv;

    //   while (kv.clear(), next_section(options_string, kv, ':')) {
    //     auto eq = kv.find_first_of("=");

    //     if (eq == kv.npos) {
    //       DebugLog(50, FMT_STRING("kv pair '{}' does not contain '='"), kv);
    //       return false;
    //     }

    //     std::string_view key = std::string_view{kv}.substr(0, eq);
    //     std::string_view value = std::string_view{kv}.substr(eq + 1);

    //     if (key.size() == 0) {
    //       DebugLog(50, FMT_STRING("kv pair '{}' does not contain a key"),
    //       kv); return false;
    //     }

    //     if (value.size() == 0) {
    //       DebugLog(
    //           50,
    //           FMT_STRING("kv pair '{}' does not contain a value (key = {})"),
    //           kv, key);
    //       return false;
    //     }

    //     DebugLog(100, FMT_STRING("{} => {}"), key, value);

    //     options.emplace_back(key, value);
    //   }

    //   if (options_string.size() > 0) {
    //     // we stopped prematurely for some reason, so just refuse to
    //     continue! DebugLog(50, FMT_STRING("premature exit detected"),
    //     options_string);

    //     return false;
    //   }
    // }

    std::optional opath = bVar::Get<bVar::PluginPath>(nullptr);
    if (!opath) {
      DebugLog(50, FMT_STRING("core could not return plugin path"));
      return false;
    }

    const char* path = *opath;

    DebugLog(10, FMT_STRING("path = {}"), path);

    std::string full_path = path;
    full_path += "/grpc/";
    full_path += name;

    child = make_connection(bareos_ctx, full_path);

    return child.has_value();
  }


  bool needs_setup() { return !child.has_value(); }


 public:
  // using option = std::pair<std::string, std::string>;

  std::string name;
  std::string cmd;
  std::string plugin_name;

  std::optional<grpc_child> child;
};

plugin_ctx* get(PluginContext* ctx)
{
  return static_cast<plugin_ctx*>(ctx->plugin_private_context);
}

bRC newPlugin(PluginContext* ctx)
{
  auto* plugin = new plugin_ctx;
  ctx->plugin_private_context = plugin;

  /* the actual setup is done inside of handle plugin event, because
   * at the moment we have no idea which plugin to start! */

  RegisterBareosEvent(ctx, filedaemon::bEventPluginCommand);
  RegisterBareosEvent(ctx, filedaemon::bEventNewPluginOptions);
  RegisterBareosEvent(ctx, filedaemon::bEventPluginCommand);
  RegisterBareosEvent(ctx, filedaemon::bEventJobStart);
  RegisterBareosEvent(ctx, filedaemon::bEventRestoreCommand);
  RegisterBareosEvent(ctx, filedaemon::bEventEstimateCommand);
  RegisterBareosEvent(ctx, filedaemon::bEventBackupCommand);
  RegisterBareosEvent(ctx, filedaemon::bEventRestoreObject);

  return bRC_OK;
}

bRC freePlugin(PluginContext* ctx)
{
  auto* plugin = get(ctx);
  delete plugin;
  return bRC_OK;
}

bRC getPluginValue(PluginContext*, filedaemon::pVariable, void*)
{
  /* UNUSED */
  return bRC_Error;
}
bRC setPluginValue(PluginContext*, filedaemon::pVariable, void*)
{
  /* UNUSED */
  return bRC_Error;
}

bRC handlePluginEvent(PluginContext* ctx, filedaemon::bEvent* event, void* data)
{
  auto* plugin = get(ctx);

  if (!plugin) {
    JobLog(ctx, M_ERROR,
           FMT_STRING("instructed to handle plugin event by core even though "
                      "context was not setup"));
    return bRC_Error;
  }

  switch (event->eventType) {
    using namespace filedaemon;
    case bEventPluginCommand: {
      if (!plugin->setup(ctx, data)) {
        JobLog(ctx, M_FATAL, FMT_STRING("could not connect to plugin"));
        return bRC_Error;
      }


      DebugLog(ctx, 100, FMT_STRING("using cmd string \"{}\" for the plugin"),
               plugin->cmd);
      // we do not want to give "grpc:" to the plugin
      return plugin->child->con.handlePluginEvent(bEventPluginCommand,
                                                  (void*)plugin->cmd.c_str());
    } break;
    case bEventNewPluginOptions:
      [[fallthrough]];
    case bEventBackupCommand:
      [[fallthrough]];
    case bEventEstimateCommand:
      [[fallthrough]];
    case bEventRestoreCommand: {
      if (!plugin->re_setup(ctx, data)) { return bRC_Error; }


      DebugLog(ctx, 100, FMT_STRING("using cmd string \"{}\" for the plugin"),
               plugin->cmd);
      // we do not want to give "grpc:" to the plugin
      return plugin->child->con.handlePluginEvent(bEventType(event->eventType),
                                                  (void*)plugin->cmd.c_str());
    } break;
    case bEventRestoreObject: {
      if (data == nullptr) {
        return plugin->child->con.handlePluginEvent(
            bEventType(event->eventType), nullptr);
      }

      auto* rop = reinterpret_cast<restore_object_pkt*>(data);
      if (!plugin->re_setup(ctx, rop->plugin_name)) { return bRC_Error; }

      char* old = rop->plugin_name;
      rop->plugin_name = const_cast<char*>(plugin->cmd.c_str());
      auto res = plugin->child->con.handlePluginEvent(
          bEventType(event->eventType), (void*)rop);
      rop->plugin_name = old;
      return res;
    } break;
    default: {
      if (plugin->needs_setup()) {
        DebugLog(
            100,
            FMT_STRING(
                "cannot handle event {} as context was not set up properly"),
            event->eventType);
        return bRC_Error;
      }
    } break;
  }

  if (!plugin->child) {
    JobLog(ctx, M_FATAL, FMT_STRING("plugin is not running"));
    return bRC_Error;
  }
  return plugin->child->con.handlePluginEvent(
      (filedaemon::bEventType)(event->eventType), data);
}

bRC startBackupFile(PluginContext* ctx, filedaemon::save_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.startBackupFile(pkt);
}
bRC endBackupFile(PluginContext* ctx)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.endBackupFile();
}
bRC startRestoreFile(PluginContext* ctx, const char* file_name)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.startRestoreFile(file_name);
}
bRC endRestoreFile(PluginContext* ctx)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.endRestoreFile();
}
bRC pluginIO(PluginContext* ctx, filedaemon::io_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.pluginIO(pkt, plugin->child->io.get());
}
bRC createFile(PluginContext* ctx, filedaemon::restore_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.createFile(pkt);
}
bRC setFileAttributes(PluginContext* ctx, filedaemon::restore_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.setFileAttributes(pkt);
}
bRC checkFile(PluginContext* ctx, char* file_name)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.checkFile(file_name);
}
bRC getAcl(PluginContext* ctx, filedaemon::acl_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.getAcl(pkt);
}
bRC setAcl(PluginContext* ctx, filedaemon::acl_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.setAcl(pkt);
}
bRC getXattr(PluginContext* ctx, filedaemon::xattr_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.getXattr(pkt);
}
bRC setXattr(PluginContext* ctx, filedaemon::xattr_pkt* pkt)
{
  auto* plugin = get(ctx);
  if (!plugin || !plugin->child) { return bRC_Error; }
  return plugin->child->con.setXattr(pkt);
}
}  // namespace

constexpr PluginInformation my_info = {
    .size = sizeof(my_info),
    .version = FD_PLUGIN_INTERFACE_VERSION,
    .plugin_magic = FD_PLUGIN_MAGIC,
    .plugin_license = "Bareos AGPLv3",
    .plugin_author = "Sebastian Sura",
    .plugin_date = "September 2024",
    .plugin_version = "0.0.1",
    .plugin_description = "a simple grpc plugin",
    .plugin_usage = "Not sure yet",
};

constexpr filedaemon::PluginFunctions my_functions = {
    .size = sizeof(my_functions),
    .version = FD_PLUGIN_INTERFACE_VERSION,
    .newPlugin = &newPlugin,
    .freePlugin = &freePlugin,
    .getPluginValue = &getPluginValue,
    .setPluginValue = &setPluginValue,
    .handlePluginEvent = &handlePluginEvent,
    .startBackupFile = &startBackupFile,
    .endBackupFile = &endBackupFile,
    .startRestoreFile = &startRestoreFile,
    .endRestoreFile = &endRestoreFile,
    .pluginIO = &pluginIO,
    .createFile = &createFile,
    .setFileAttributes = &setFileAttributes,
    .checkFile = &checkFile,
    .getAcl = &getAcl,
    .setAcl = &setAcl,
    .getXattr = &getXattr,
    .setXattr = &setXattr,
};

bool AmICompatibleWith(filedaemon::PluginApiDefinition* core_info)
{
  DebugLog(100, FMT_STRING("size = {}/{},  version = {}/{}"), core_info->size,
           sizeof(*core_info), core_info->version, FD_PLUGIN_INTERFACE_VERSION);

  if (core_info->size != sizeof(*core_info)
      || core_info->version != FD_PLUGIN_INTERFACE_VERSION) {
    return false;
  }

  return true;
}

extern "C" int loadPlugin(filedaemon::PluginApiDefinition* core_info,
                          filedaemon::CoreFunctions* core_funcs,
                          PluginInformation** plugin_info,
                          filedaemon::PluginFunctions** plugin_funcs)
{
  SetupBareosApi(core_funcs);

  if (!AmICompatibleWith(core_info)) {
    DebugLog(10,
             FMT_STRING("ABI mismatch detected.  Cannot load plugin.  Expected "
                        "abi version = {}"),
             FD_PLUGIN_INTERFACE_VERSION);
    return -1;
  }

  *plugin_info = const_cast<PluginInformation*>(&my_info);
  *plugin_funcs = const_cast<filedaemon::PluginFunctions*>(&my_functions);

  DebugLog(100, FMT_STRING("plugin loaded successfully"));

  return 0;
}

extern "C" int unloadPlugin() { return 0; }
