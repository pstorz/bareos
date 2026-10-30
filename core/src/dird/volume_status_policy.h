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

#ifndef BAREOS_DIRD_VOLUME_STATUS_POLICY_H_
#define BAREOS_DIRD_VOLUME_STATUS_POLICY_H_

#include <string_view>

namespace directordaemon {

/*
 * The director owns the volume status. The storage daemon sends the status
 * from its cached copy of the catalog record, which may be stale. Only the
 * transitions the storage daemon observes itself are accepted:
 *  - a (re)label writes a fresh label: Append
 *  - end of medium: Full
 *  - I/O or label errors: Error
 *  - data written to a manually created Unlabeled volume: Append
 * Every other reported status is ignored and the catalog status is kept.
 */
constexpr std::string_view ResolveVolumeStatus(std::string_view catalog,
                                               std::string_view reported,
                                               bool relabeled)
{
  if (reported == catalog) { return catalog; }
  if (relabeled && reported == "Append") { return reported; }
  if (reported == "Error") { return reported; }
  if (reported == "Full" && catalog != "Error") { return reported; }
  if (reported == "Append" && catalog == "Unlabeled") { return reported; }
  return catalog;
}

}  // namespace directordaemon

#endif  // BAREOS_DIRD_VOLUME_STATUS_POLICY_H_
