/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
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

#ifndef BAREOS_STORED_AUTOCHANGER_RESOURCE_H_
#define BAREOS_STORED_AUTOCHANGER_RESOURCE_H_

#include "lib/bareos_resource.h"

#include <string>
#include <memory>

template <typename T> class alist;

namespace storagedaemon {
class DeviceResource;

class AutochangerResource : public BareosResource {
 public:
  AutochangerResource();

  /* Create an implicit autochanger for the given device name. The
   * `device_resources` list is pre-allocated with capacity for
   * `initial_device_capacity` entries so that it never needs to grow
   * (reallocate) afterwards: this matters for autochangers backed by a
   * multiplied device, whose device list is appended to at runtime (see
   * reserve.cc SpawnMultipliedDevice()) from a job-handling thread while
   * other threads (status reporting, statistics, Director device lookup)
   * iterate the same list without taking a lock. Passing a capacity that
   * covers the maximum possible number of entries up front avoids a
   * use-after-free on the `alist`'s backing array that a runtime
   * reallocation would otherwise cause for those lock-free readers. */
  static std::unique_ptr<AutochangerResource> CreateImplicitAutochanger(
      const std::string& device_name,
      int initial_device_capacity = 10);

  virtual ~AutochangerResource() = default;
  AutochangerResource& operator=(const AutochangerResource& rhs);
  bool PrintConfig(OutputFormatterResource& send,
                   const ConfigurationParser&,
                   bool hide_sensitive_data,
                   bool verbose = false) override;


  alist<DeviceResource*>* device_resources{
      nullptr};                   /**< List of DeviceResource device pointers */
  char* changer_name{nullptr};    /**< Changer device name */
  char* changer_command{nullptr}; /**< Changer command  -- external program */
  brwlock_t changer_lock;         /**< One changer operation at a time */
  DeviceResource* multiplied_device_template{
      nullptr}; /**< If set, this autochanger was implicitly created by a
                   Device's Count directive; points at the (`$`-prefixed)
                   template device that new devices are lazily copied from
                   on demand, up to the template's `count` cap. */
 private:
  bool implicitly_created_{false};
};
} /* namespace storagedaemon */

#endif  // BAREOS_STORED_AUTOCHANGER_RESOURCE_H_
