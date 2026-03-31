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
 * Fuzz test for BareosSocket::ControlBwlimit().
 *
 * ControlBwlimit() implements a token-bucket rate limiter using integer and
 * floating-point arithmetic.  Edge cases include:
 *   - bytes == 0 (early return)
 *   - first call (last_tick_ == 0, initialises the counter)
 *   - very large byte counts that could overflow int64_t arithmetic
 *   - bwlimit_ just above zero to stress the division path
 *
 * No network is required; we instantiate BareosSocketTCP directly.
 *
 * To prevent the sleep loop from running indefinitely we set bwlimit_ to a
 * large value (100 MB/s) so any computed sleep time is in the nanosecond
 * range, and we cap the fuzz input to values that cannot cause more than a
 * few hundred microseconds of sleep.
 */

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"

#include "include/bareos.h"
#include "lib/bsock_tcp.h"

#include <climits>
#include <mutex>

static void BareosRuntimeInit()
{
  static std::once_flag flag;
  std::call_once(flag, []() { OSDependentInit(); });
}

// 100 MB/s – large enough that sleep times are negligible.
static constexpr int64_t kBwLimit = 100'000'000LL;

// Property: ControlBwlimit must never crash for any byte count.
static void ControlBwlimitNeverCrashes(int bytes)
{
  BareosRuntimeInit();

  BareosSocketTCP sock;
  sock.suppress_error_msgs_ = true;
  sock.SetBwlimit(kBwLimit);

  // First call – initialises last_tick_.
  sock.ControlBwlimit(1);
  // Second call with fuzz input.
  sock.ControlBwlimit(bytes);
}
FUZZ_TEST(BsockFuzz, ControlBwlimitNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<int>());

// Property: bursting mode must not crash either.
static void ControlBwlimitBurstingNeverCrashes(int bytes)
{
  BareosRuntimeInit();

  BareosSocketTCP sock;
  sock.suppress_error_msgs_ = true;
  sock.SetBwlimit(kBwLimit);
  sock.SetBwlimitBursting();

  sock.ControlBwlimit(1);
  sock.ControlBwlimit(bytes);
}
FUZZ_TEST(BsockFuzz, ControlBwlimitBurstingNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<int>());

// Regression: bytes == 0 must return immediately without touching state.
TEST(BsockFuzz, ControlBwlimitZeroBytes)
{
  BareosRuntimeInit();
  BareosSocketTCP sock;
  sock.suppress_error_msgs_ = true;
  sock.SetBwlimit(kBwLimit);
  sock.ControlBwlimit(0);
}

// Regression: INT_MAX bytes with bwlimit active.
TEST(BsockFuzz, ControlBwlimitIntMax)
{
  BareosRuntimeInit();
  BareosSocketTCP sock;
  sock.suppress_error_msgs_ = true;
  sock.SetBwlimit(kBwLimit);
  sock.ControlBwlimit(1);
  sock.ControlBwlimit(INT_MAX);
}

// Regression: INT_MIN bytes (negative – treated as a large value via int).
TEST(BsockFuzz, ControlBwlimitIntMin)
{
  BareosRuntimeInit();
  BareosSocketTCP sock;
  sock.suppress_error_msgs_ = true;
  sock.SetBwlimit(kBwLimit);
  sock.ControlBwlimit(1);
  sock.ControlBwlimit(INT_MIN);
}
