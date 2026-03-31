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
 * Fuzz test for BareosSocket::send(const char*, uint32_t) and the recv() path.
 *
 * The send/recv round-trip exercises:
 *   - buffer allocation (msg is resized to fit nbytes)
 *   - the 4-byte header framing written by send()
 *   - the header-then-body read sequence in recv()
 *
 * Interesting inputs: length=0, length >> data size, very large payloads,
 * data with embedded NUL bytes, data with all-0xFF bytes.
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

// Property: send(data, nbytes) followed by recv() must never crash.
// nbytes is capped at 1 MB – 4 (max_message_len) to stay within a single
// Bareos packet so the fuzzer does not generate enormous allocations.
static void SendRecvNeverCrashes(std::string data, uint32_t nbytes)
{
  BareosRuntimeInit();

  static constexpr uint32_t kMaxMsgLen = 999996;
  nbytes = nbytes % (kMaxMsgLen + 1);

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { return; }

  // Sender socket
  BareosSocketTCP sender;
  sender.fd_ = sv[0];
  sender.SetWho(strdup("fuzz-sender"));
  sender.suppress_error_msgs_ = true;

  // Receiver socket
  BareosSocketTCP receiver;
  receiver.fd_ = sv[1];
  receiver.SetWho(strdup("fuzz-receiver"));
  receiver.suppress_error_msgs_ = true;

  // send() copies nbytes from data; data may be shorter – that is fine,
  // the function reads from uninitialized memory in that case which is UB
  // but a real-world scenario (buffer overread).  Pad to nbytes to stay safe.
  std::string padded = data;
  if (padded.size() < nbytes) { padded.resize(nbytes, '\0'); }

  static_cast<BareosSocket&>(sender).send(padded.data(), nbytes);
  // Close write end so receiver sees EOF if it tries to read past the message.
  sender.fd_ = -1;
  close(sv[0]);

  receiver.recv();

  receiver.fd_ = -1;
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, SendRecvNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::string>(),
                 fuzztest::Arbitrary<uint32_t>());

// Property: the round-trip preserves the message content for valid sends.
static void SendRecvRoundTrip(std::string data)
{
  BareosRuntimeInit();

  if (data.empty()) { return; }
  if (data.size() > 999996) { data.resize(999996); }

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

  bool sent = static_cast<BareosSocket&>(sender).send(data.data(), static_cast<uint32_t>(data.size()));

  if (sent) {
    int32_t n = receiver.recv();
    if (n > 0) {
      EXPECT_EQ(static_cast<size_t>(n), data.size());
      EXPECT_EQ(std::memcmp(receiver.msg, data.data(), data.size()), 0);
    }
  }

  sender.fd_ = -1;
  receiver.fd_ = -1;
  close(sv[0]);
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, SendRecvRoundTrip)
    .WithDomains(fuzztest::Arbitrary<std::string>());

// Regression: zero-length send.
TEST(BsockFuzz, SendRecvZeroLength)
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

  static_cast<BareosSocket&>(sender).send("", 0);
  sender.fd_ = -1;
  close(sv[0]);
  receiver.recv();
  receiver.fd_ = -1;
  close(sv[1]);
}
