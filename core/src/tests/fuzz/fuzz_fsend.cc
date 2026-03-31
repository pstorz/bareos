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
 * Fuzz test for BareosSocket::fsend() / vfsend().
 *
 * fsend() and vfsend() format a message via PmVFormat() into the POOLMEM msg
 * buffer then call send().  We fuzz the string argument that gets passed as
 * the %s substitution so arbitrary byte sequences go through PmVFormat and
 * the TCP framing layer.
 *
 * We also exercise the path where we read the sent message back with recv().
 */

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"

#include "include/bareos.h"
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

// Property: fsend("%s", arbitrary_string) must never crash.
static void FsendNeverCrashes(std::string data)
{
  BareosRuntimeInit();

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { return; }

  BareosSocketTCP sender;
  sender.fd_ = sv[0];
  sender.SetWho(strdup("fuzz-sender"));
  sender.suppress_error_msgs_ = true;

  BareosSocketTCP receiver;
  receiver.fd_ = sv[1];
  receiver.SetWho(strdup("fuzz-receiver"));
  receiver.suppress_error_msgs_ = true;

  static_cast<BareosSocket&>(sender).fsend("%s", data.c_str());

  sender.fd_ = -1;
  close(sv[0]);

  receiver.recv();

  receiver.fd_ = -1;
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, FsendNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>());

// Property: the received message equals the sent string (when it fits in one
// packet and contains no embedded NULs that would truncate the C-string).
static void FsendRoundTrip(std::string data)
{
  BareosRuntimeInit();

  // NUL bytes would truncate the C-string passed to fsend; exclude them.
  // Also cap to max_message_len - some overhead for the format string.
  data.erase(std::remove(data.begin(), data.end(), '\0'), data.end());
  if (data.size() > 999990) { data.resize(999990); }
  if (data.empty()) { return; }

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { return; }

  BareosSocketTCP sender, receiver;
  sender.fd_ = sv[0];
  sender.SetWho(strdup("s"));
  sender.suppress_error_msgs_ = true;
  receiver.fd_ = sv[1];
  receiver.SetWho(strdup("r"));
  receiver.suppress_error_msgs_ = true;

  bool sent = static_cast<BareosSocket&>(sender).fsend("%s", data.c_str());
  if (sent) {
    int32_t n = receiver.recv();
    if (n > 0) {
      EXPECT_EQ(std::string(receiver.msg, static_cast<size_t>(n)), data);
    }
  }

  sender.fd_ = -1;
  receiver.fd_ = -1;
  close(sv[0]);
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, FsendRoundTrip)
    .WithDomains(fuzztest::Arbitrary<std::string>());

// Regression: empty string.
TEST(BsockFuzz, FsendEmptyString)
{
  BareosRuntimeInit();
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  BareosSocketTCP sender, receiver;
  sender.fd_ = sv[0];
  sender.SetWho(strdup("s"));
  sender.suppress_error_msgs_ = true;
  receiver.fd_ = sv[1];
  receiver.SetWho(strdup("r"));
  receiver.suppress_error_msgs_ = true;

  static_cast<BareosSocket&>(sender).fsend("%s", "");
  sender.fd_ = -1;
  close(sv[0]);
  receiver.recv();
  receiver.fd_ = -1;
  close(sv[1]);
}
