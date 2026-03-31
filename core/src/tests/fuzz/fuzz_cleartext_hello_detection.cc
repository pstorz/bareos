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
 * Fuzz test for BareosSocket::EvaluateCleartextBareosHello().
 *
 * The function reads up to 255 bytes from the socket using MSG_PEEK and checks
 * whether the connection is a cleartext "Hello …" greeting or a TLS ClientHello.
 * A socketpair(AF_UNIX) feeds arbitrary bytes so MSG_PEEK has data to inspect.
 */

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"

#include "include/bareos.h"
#include "include/version_numbers.h"
#include "lib/bsock_tcp.h"

#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static void BareosRuntimeInit()
{
  static std::once_flag flag;
  std::call_once(flag, []() { OSDependentInit(); });
}

// Property: EvaluateCleartextBareosHello must never crash for any byte sequence.
static void CleartextHelloNeverCrashes(std::string wire_data)
{
  BareosRuntimeInit();

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { return; }

  // Write data and keep the write end open so MSG_PEEK doesn't see EOF.
  if (!wire_data.empty()) {
    (void)write(sv[0], wire_data.data(), wire_data.size());
  }

  BareosSocketTCP sock;
  sock.fd_ = sv[1];
  sock.SetWho(strdup("fuzz"));
  sock.suppress_error_msgs_ = true;

  bool cleartext = false;
  std::string client_name;
  std::string r_code_str;
  BareosVersionNumber version = BareosVersionNumber::kUndefined;

  sock.EvaluateCleartextBareosHello(cleartext, client_name, r_code_str,
                                    version);

  sock.fd_ = -1;
  close(sv[0]);
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, CleartextHelloNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>());

// Regression: empty socket (no data) must not crash.
TEST(BsockFuzz, CleartextHelloEmptySocket)
{
  BareosRuntimeInit();
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  // Close the write end immediately so MSG_PEEK sees EOF (0 bytes) rather
  // than blocking indefinitely waiting for data.
  close(sv[0]);

  BareosSocketTCP sock;
  sock.fd_ = sv[1];
  sock.SetWho(strdup("fuzz"));
  sock.suppress_error_msgs_ = true;

  bool cleartext = false;
  std::string name, code;
  BareosVersionNumber ver = BareosVersionNumber::kUndefined;
  sock.EvaluateCleartextBareosHello(cleartext, name, code, ver);

  sock.fd_ = -1;
  close(sv[1]);
}

// Regression: a valid cleartext hello must be detected.
TEST(BsockFuzz, CleartextHelloValidGreeting)
{
  BareosRuntimeInit();
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  // Prepend a 4-byte header (the function skips the first 4 bytes).
  const char header[4] = {0, 0, 0, 0};
  const char hello[] = "Hello bareos-dir calling version 18.2.5\n";
  write(sv[0], header, 4);
  write(sv[0], hello, sizeof(hello) - 1);

  BareosSocketTCP sock;
  sock.fd_ = sv[1];
  sock.SetWho(strdup("fuzz"));
  sock.suppress_error_msgs_ = true;

  bool cleartext = false;
  std::string name, code;
  BareosVersionNumber ver = BareosVersionNumber::kUndefined;
  bool ok = sock.EvaluateCleartextBareosHello(cleartext, name, code, ver);

  EXPECT_TRUE(ok);
  EXPECT_TRUE(cleartext);
  EXPECT_EQ(name, "bareos-dir");

  sock.fd_ = -1;
  close(sv[0]);
  close(sv[1]);
}
