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

#include "bareos_api.h"

struct globals {
  const filedaemon::CoreFunctions* core{nullptr};
};

static globals fd{};

void SetupBareosApi(const filedaemon::CoreFunctions* core) { fd.core = core; }

namespace internal {
void DebugMessage(/* optional */ PluginContext* ctx,
                  const char* file,
                  int line,
                  int level,
                  const char* string)
{
  if (fd.core) {
    fd.core->DebugMessage(ctx, file, line, level, "%s\n", string);
  }
}

void JobMessage(PluginContext* ctx,
                const char* file,
                int line,
                int type,
                const char* string)
{
  if (fd.core) {
    fd.core->JobMessage(ctx, file, line, type, 0, "%s\n", string);
  }
}
};  // namespace internal

void RegisterBareosEvent(PluginContext* ctx, filedaemon::bEventType event)
{
  if (fd.core) { fd.core->registerBareosEvents(ctx, 1, event); }
}

void SetBareosValue(PluginContext* ctx, filedaemon::bVariable var, void* value)
{
  if (fd.core) { fd.core->setBareosValue(ctx, var, value); }
}

void GetBareosValue(PluginContext* ctx, filedaemon::bVariable var, void* value)
{
  if (fd.core) { fd.core->getBareosValue(ctx, var, value); }
}
