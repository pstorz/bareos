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
 * Fuzz tests for the response-message protocol parsing functions:
 *   - EvaluateResponseMessageId()
 *   - ReadoutCommandIdFromMessage()
 *
 * These functions parse the RS-delimited "ID\x1earg1\x1earg2…" messages
 * exchanged during connection setup.  Both take plain std::string / BStringList
 * inputs, so no network setup is required.
 *
 * Requires BAREOS_TEST_LIB compile definition to expose the functions in
 * bnet.h.
 */

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"

#include "include/bareos.h"
#include "lib/bnet.h"
#include "lib/bstringlist.h"
#include "lib/ascii_control_characters.h"

#include <mutex>
#include <string>

static void BareosRuntimeInit()
{
  static std::once_flag flag;
  std::call_once(flag, []() { OSDependentInit(); });
}

// Property: EvaluateResponseMessageId must never crash for any input.
static void EvaluateResponseMessageNeverCrashes(std::string input)
{
  BareosRuntimeInit();

  uint32_t id = kMessageIdUnknown;
  BStringList args;
  EvaluateResponseMessageId(input, id, args);
}
FUZZ_TEST(BsockFuzz, EvaluateResponseMessageNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>());

// Property: ReadoutCommandIdFromMessage must never crash for any BStringList.
static void ReadoutCommandIdNeverCrashes(std::vector<std::string> parts)
{
  BareosRuntimeInit();

  BStringList list;
  for (const auto& p : parts) { list << p; }

  uint32_t id = kMessageIdUnknown;
  ReadoutCommandIdFromMessage(list, id);
}
FUZZ_TEST(BsockFuzz, ReadoutCommandIdNeverCrashes)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<std::string>())
                     .WithMaxSize(32));

// Regression: empty string.
TEST(BsockFuzz, EvaluateResponseMessageEmpty)
{
  BareosRuntimeInit();
  uint32_t id = kMessageIdUnknown;
  BStringList args;
  EvaluateResponseMessageId("", id, args);
}

// Regression: well-formed OK response.
TEST(BsockFuzz, EvaluateResponseMessageOk)
{
  BareosRuntimeInit();
  uint32_t id = kMessageIdUnknown;
  BStringList args;
  std::string msg = std::to_string(kMessageIdOk);
  msg += AsciiControlCharacters::RecordSeparator();
  msg += "some_argument";
  bool ok = EvaluateResponseMessageId(msg, id, args);
  EXPECT_TRUE(ok);
  EXPECT_EQ(id, kMessageIdOk);
}
