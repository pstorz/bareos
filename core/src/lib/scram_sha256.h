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
 * @file
 * SCRAM-SHA-256 mutual authentication (RFC 5802 / RFC 7677).
 *
 * Replaces CRAM-MD5 for clients with version >= kRelease_26_0.
 * Authentication always runs inside an established TLS tunnel.
 */

#ifndef BAREOS_LIB_SCRAM_SHA256_H_
#define BAREOS_LIB_SCRAM_SHA256_H_

#include <cstdint>
#include <string>
#include <string_view>

class BareosSocket;

// Stored password verifier — serialised into the config file as:
//   scram-sha-256:i=<iter>,s=<salt-b64>,sk=<StoredKey-b64>,svk=<ServerKey-b64>
struct ScramSha256Verifier {
  int iterations{310000};
  std::string salt;        // raw bytes
  std::string stored_key;  // H(ClientKey), raw bytes
  std::string server_key;  // HMAC(SaltedPw,"Server Key"), raw bytes

  // Produce the config-file string representation.
  std::string Serialize() const;

  // Parse a config-file string (the part after "scram-sha-256:").
  // Returns false if the format is invalid.
  static bool Deserialize(std::string_view encoded,
                          ScramSha256Verifier& out_verifier);
};

// Generate a fresh verifier from a plaintext password.
// Uses a random 16-byte salt and the specified iteration count.
ScramSha256Verifier GenerateScramSha256Verifier(std::string_view password,
                                                int iterations = 310000);

/**
 * Implements the SCRAM-SHA-256 handshake for one side of a connection.
 *
 * Usage mirrors CramMd5Handshake:
 *
 *   // Server side (initiated_by_remote = true):
 *   ScramSha256Handshake hs(bs, verifier, own_name);
 *   bool ok = hs.DoHandshake(true);
 *
 *   // Client side (initiated_by_remote = false):
 *   ScramSha256Handshake hs(bs, password, own_name);
 *   bool ok = hs.DoHandshake(false);
 *
 * The server is constructed with a ScramSha256Verifier (no plaintext
 * password needed).  The client is constructed with the plaintext
 * password and derives all keys on the fly during the handshake.
 */
class ScramSha256Handshake {
 public:
  enum class HandshakeResult
  {
    NOT_INITIALIZED,
    SUCCESS,
    FORMAT_MISMATCH,
    NETWORK_ERROR,
    WRONG_HASH,
  };

  mutable HandshakeResult result{HandshakeResult::NOT_INITIALIZED};

  // Server-side constructor: uses stored verifier, no plaintext password.
  ScramSha256Handshake(BareosSocket* bs,
                       const ScramSha256Verifier& verifier,
                       std::string_view own_qualified_name);

  // Client-side constructor: uses plaintext password.
  ScramSha256Handshake(BareosSocket* bs,
                       std::string_view password,
                       std::string_view own_qualified_name);

  // Run the full 4-frame exchange.
  // initiated_by_remote = true  → this side is the server (sends challenge)
  // initiated_by_remote = false → this side is the client (sends response)
  bool DoHandshake(bool initiated_by_remote);

 private:
  static constexpr int kDebugLevel = 50;
  static constexpr int kAuthTimeoutSecs = 180;

  BareosSocket* bs_;
  std::string own_qualified_name_;

  // Server side holds verifier; client side holds plaintext password.
  bool is_server_{false};
  ScramSha256Verifier verifier_{};  // server only
  std::string password_{};          // client only

  bool ServerSide();
  bool ClientSide();
};

#endif  // BAREOS_LIB_SCRAM_SHA256_H_
