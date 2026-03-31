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
/**
 * Fuzz test for GetNameAndResourceTypeAndVersionFromHello().
 *
 * This pure parsing function accepts the cleartext "Hello …" greeting that
 * clients send during connection setup.  It takes an arbitrary std::string so
 * no network setup is needed.
 */

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"

#include "include/bareos.h"
#include "lib/util.h"
#include "include/version_numbers.h"

#include <mutex>
#include <string>

static void BareosRuntimeInit()
{
  static std::once_flag flag;
  std::call_once(flag, []() { OSDependentInit(); });
}

// Property: the function must never crash for any input.
static void HelloParsingNeverCrashes(std::string input)
{
  BareosRuntimeInit();

  std::string name;
  std::string r_type_str;
  BareosVersionNumber version = BareosVersionNumber::kUndefined;

  /* Return value is intentionally ignored – we only care that it doesn't
   * crash, abort, or invoke UB. */
  GetNameAndResourceTypeAndVersionFromHello(input, name, r_type_str, version);
}
FUZZ_TEST(BsockFuzz, HelloParsingNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>());

// Regression: empty string must not crash.
TEST(BsockFuzz, HelloParsingEmptyString)
{
  BareosRuntimeInit();
  std::string name, r_type_str;
  BareosVersionNumber version = BareosVersionNumber::kUndefined;
  GetNameAndResourceTypeAndVersionFromHello("", name, r_type_str, version);
}

// Regression: valid hello string must parse correctly.
TEST(BsockFuzz, HelloParsingValidHello)
{
  BareosRuntimeInit();
  std::string name, r_type_str;
  BareosVersionNumber version = BareosVersionNumber::kUndefined;
  bool ok = GetNameAndResourceTypeAndVersionFromHello(
      "Hello bareos-dir calling version 18.2.5\n", name, r_type_str, version);
  EXPECT_TRUE(ok);
  EXPECT_EQ(name, "bareos-dir");
}
