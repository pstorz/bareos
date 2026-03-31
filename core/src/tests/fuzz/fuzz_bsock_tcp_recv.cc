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
 * Fuzz test for BareosSocketTCP::recv().
 *
 * The wire format is: 4-byte big-endian length header followed by that many
 * bytes of payload.  Negative headers encode signals (BNET_*).
 *
 * A socketpair(AF_UNIX) is used to feed arbitrary bytes into recv() without
 * a real TCP connection.  The write end is closed before recv() is called so
 * the reader sees EOF if it requests more data than was written.
 */

#include "gtest/gtest.h"
#include "fuzztest/fuzztest.h"

#include "include/bareos.h"
#include "lib/bsock_tcp.h"

#include <arpa/inet.h>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static void BareosRuntimeInit()
{
  static std::once_flag flag;
  std::call_once(flag, []() { OSDependentInit(); });
}

// Build a valid Bareos TCP packet from raw payload bytes.
static std::vector<uint8_t> MakePacket(const std::vector<uint8_t>& payload)
{
  std::vector<uint8_t> pkt(4 + payload.size());
  int32_t len = static_cast<int32_t>(payload.size());
  int32_t net_len;
  std::memcpy(&net_len, &len, sizeof(net_len));
  net_len = htonl(net_len);
  std::memcpy(pkt.data(), &net_len, 4);
  std::memcpy(pkt.data() + 4, payload.data(), payload.size());
  return pkt;
}

// Property: recv() must never crash regardless of the bytes on the wire.
static void RecvNeverCrashes(std::vector<uint8_t> wire_bytes)
{
  BareosRuntimeInit();

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { return; }

  // Write all fuzz data then close the write end so recv() sees EOF.
  if (!wire_bytes.empty()) {
    (void)write(sv[0], wire_bytes.data(), wire_bytes.size());
  }
  close(sv[0]);

  BareosSocketTCP sock;
  sock.fd_ = sv[1];
  sock.SetWho(strdup("fuzz"));
  sock.suppress_error_msgs_ = true;

  sock.recv();

  sock.fd_ = -1;  // Prevent double-close: destroy() will skip it.
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, RecvNeverCrashes)
    .WithDomains(fuzztest::Arbitrary<std::vector<uint8_t>>());

// Property: recv() on a well-formed packet returns the payload length.
static void RecvWellFormedPacketReturnsLength(std::vector<uint8_t> payload)
{
  BareosRuntimeInit();

  // Limit payload to max_packet_size - 4 so it fits in a single packet.
  if (payload.size() > 999996) { payload.resize(999996); }

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { return; }

  auto pkt = MakePacket(payload);
  (void)write(sv[0], pkt.data(), pkt.size());
  close(sv[0]);

  BareosSocketTCP sock;
  sock.fd_ = sv[1];
  sock.SetWho(strdup("fuzz"));
  sock.suppress_error_msgs_ = true;

  int32_t n = sock.recv();

  if (!payload.empty()) { EXPECT_EQ(n, static_cast<int32_t>(payload.size())); }

  sock.fd_ = -1;
  close(sv[1]);
}
FUZZ_TEST(BsockFuzz, RecvWellFormedPacketReturnsLength)
    .WithDomains(fuzztest::Arbitrary<std::vector<uint8_t>>());

// Regression: signal packet (BNET_EOD = -1).
TEST(BsockFuzz, RecvSignalPacket)
{
  BareosRuntimeInit();
  int sv[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  int32_t net_sig = htonl(-1);
  write(sv[0], &net_sig, 4);
  close(sv[0]);

  BareosSocketTCP sock;
  sock.fd_ = sv[1];
  sock.SetWho(strdup("fuzz"));
  sock.suppress_error_msgs_ = true;

  int32_t n = sock.recv();
  EXPECT_EQ(n, BNET_SIGNAL);

  sock.fd_ = -1;
  close(sv[1]);
}
