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
 * SCRAM-SHA-256 mutual authentication — RFC 5802 / RFC 7677.
 *
 * Wire protocol (all frames inside an established TLS tunnel):
 *
 *   Server → Client: "auth-methods scram-sha-256\n"
 *   Client → Server: "auth-select scram-sha-256\n"
 *   Client → Server: "n,,n=<username>,r=<cnonce>"         (client-first)
 *   Server → Client: "r=<cnonce><snonce>,s=<salt-b64>,i=<N>"  (server-first)
 *   Client → Server: "c=biws,r=<nonces>,p=<ClientProof-b64>"  (client-final)
 *   Server → Client: "v=<ServerSignature-b64>"             (server-final)
 *   Server → Client: "1000 OK auth\n"  or  "1999 Authorization failed.\n"
 *
 * Key derivations (RFC 5802 §3):
 *   SaltedPassword  = PBKDF2-SHA-256(password, salt, i, 32)
 *   ClientKey       = HMAC-SHA-256(SaltedPassword, "Client Key")
 *   StoredKey       = SHA-256(ClientKey)
 *   ServerKey       = HMAC-SHA-256(SaltedPassword, "Server Key")
 *   AuthMessage     = client-first-bare + "," + server-first
 *                     + "," + client-final-without-proof
 *   ClientSignature = HMAC-SHA-256(StoredKey, AuthMessage)
 *   ClientProof     = ClientKey XOR ClientSignature
 *   ServerSignature = HMAC-SHA-256(ServerKey, AuthMessage)
 */

#include "include/bareos.h"
#include "lib/scram_sha256.h"
#include "lib/bsock.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/buffer.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static constexpr int kSha256Len = 32;

// Standard RFC 4648 base64 (not the Bacula-compatible variant).
static std::string Base64Encode(const std::string& data)
{
  if (data.empty()) { return {}; }

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* mem = BIO_new(BIO_s_mem());
  BIO_push(b64, mem);
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(b64, data.data(), static_cast<int>(data.size()));
  BIO_flush(b64);

  BUF_MEM* buf{};
  BIO_get_mem_ptr(mem, &buf);
  std::string result(buf->data, buf->length);
  BIO_free_all(b64);
  return result;
}

static std::string Base64Decode(const std::string& encoded)
{
  if (encoded.empty()) { return {}; }

  BIO* b64 = BIO_new(BIO_f_base64());
  BIO* mem = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
  BIO_push(b64, mem);
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

  std::string result(encoded.size(), '\0');
  int len
      = BIO_read(b64, result.data(), static_cast<int>(encoded.size()));
  BIO_free_all(b64);
  if (len < 0) { return {}; }
  result.resize(static_cast<size_t>(len));
  return result;
}

// HMAC-SHA-256(key, data) → 32-byte string
static std::string HmacSha256(const std::string& key, const std::string& data)
{
  std::array<uint8_t, kSha256Len> out{};
  unsigned int out_len = kSha256Len;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const uint8_t*>(data.data()),
       static_cast<int>(data.size()), out.data(), &out_len);
  return std::string(reinterpret_cast<char*>(out.data()), out_len);
}

// SHA-256(data) → 32-byte string
static std::string Sha256(const std::string& data)
{
  std::array<uint8_t, kSha256Len> out{};
  SHA256(reinterpret_cast<const uint8_t*>(data.data()),
         static_cast<int>(data.size()), out.data());
  return std::string(reinterpret_cast<char*>(out.data()), kSha256Len);
}

// PBKDF2-SHA-256(password, salt, iterations, 32) → 32-byte string
static std::string Pbkdf2Sha256(const std::string& password,
                                const std::string& salt,
                                int iterations)
{
  std::array<uint8_t, kSha256Len> out{};
  PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                    reinterpret_cast<const uint8_t*>(salt.data()),
                    static_cast<int>(salt.size()), iterations, EVP_sha256(),
                    kSha256Len, out.data());
  return std::string(reinterpret_cast<char*>(out.data()), kSha256Len);
}

// XOR two equal-length byte strings in place: a ^= b
static void XorBytes(std::string& a, const std::string& b)
{
  for (size_t i = 0; i < a.size(); ++i) {
    a[i] = static_cast<char>(static_cast<uint8_t>(a[i])
                             ^ static_cast<uint8_t>(b[i]));
  }
}

// Generate a cryptographically random printable nonce (~22 chars of base64).
static std::string GenerateNonce()
{
  std::array<uint8_t, 16> raw{};
  RAND_bytes(raw.data(), static_cast<int>(raw.size()));
  return Base64Encode(
      std::string(reinterpret_cast<char*>(raw.data()), raw.size()));
}

// ---------------------------------------------------------------------------
// ScramSha256Verifier
// ---------------------------------------------------------------------------

ScramSha256Verifier GenerateScramSha256Verifier(std::string_view password,
                                                int iterations)
{
  ScramSha256Verifier v;
  v.iterations = iterations;

  // Random 16-byte salt
  std::array<uint8_t, 16> salt_raw{};
  RAND_bytes(salt_raw.data(), static_cast<int>(salt_raw.size()));
  v.salt = std::string(reinterpret_cast<char*>(salt_raw.data()),
                       salt_raw.size());

  std::string pw(password);
  std::string salted_password = Pbkdf2Sha256(pw, v.salt, v.iterations);
  std::string client_key = HmacSha256(salted_password, "Client Key");
  v.stored_key = Sha256(client_key);
  v.server_key = HmacSha256(salted_password, "Server Key");
  return v;
}

std::string ScramSha256Verifier::Serialize() const
{
  std::ostringstream oss;
  oss << "i=" << iterations << ",s=" << Base64Encode(salt)
      << ",sk=" << Base64Encode(stored_key)
      << ",svk=" << Base64Encode(server_key);
  return oss.str();
}

bool ScramSha256Verifier::Deserialize(std::string_view encoded,
                                      ScramSha256Verifier& v)
{
  // Expected: "i=<N>,s=<b64>,sk=<b64>,svk=<b64>"
  std::string s(encoded);
  auto extract = [&](const std::string& prefix,
                     const std::string& next_key) -> std::string {
    auto pos = s.find(prefix + "=");
    if (pos == std::string::npos) { return {}; }
    pos += prefix.size() + 1;
    auto end = next_key.empty() ? std::string::npos : s.find("," + next_key + "=", pos);
    return s.substr(pos, end == std::string::npos ? end : end - pos);
  };

  std::string iter_str = extract("i", "s");
  std::string salt_b64 = extract("s", "sk");
  std::string sk_b64 = extract("sk", "svk");
  std::string svk_b64 = extract("svk", "");

  if (iter_str.empty() || salt_b64.empty() || sk_b64.empty()
      || svk_b64.empty()) {
    return false;
  }

  try {
    v.iterations = std::stoi(iter_str);
  } catch (...) {
    return false;
  }
  v.salt = Base64Decode(salt_b64);
  v.stored_key = Base64Decode(sk_b64);
  v.server_key = Base64Decode(svk_b64);
  return !v.salt.empty() && !v.stored_key.empty() && !v.server_key.empty();
}

// ---------------------------------------------------------------------------
// ScramSha256Handshake — constructors
// ---------------------------------------------------------------------------

ScramSha256Handshake::ScramSha256Handshake(
    BareosSocket* bs,
    const ScramSha256Verifier& verifier,
    std::string_view own_qualified_name)
    : bs_(bs)
    , own_qualified_name_(own_qualified_name)
    , is_server_(true)
    , verifier_(verifier)
{
}

ScramSha256Handshake::ScramSha256Handshake(BareosSocket* bs,
                                           std::string_view password,
                                           std::string_view own_qualified_name)
    : bs_(bs)
    , own_qualified_name_(own_qualified_name)
    , is_server_(false)
    , password_(password)
{
}

// ---------------------------------------------------------------------------
// Server side
// ---------------------------------------------------------------------------

bool ScramSha256Handshake::ServerSide()
{
  // --- Receive client-first-message ---
  if (bs_->WaitData(kAuthTimeoutSecs) <= 0 || bs_->recv() <= 0) {
    Dmsg1(kDebugLevel, "scram: recv client-first failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }
  std::string client_first(bs_->msg);

  // client-first: "n,,n=<username>,r=<cnonce>"
  // client-first-bare is everything after the leading "n,,"
  static const std::string kGs2Header = "n,,";
  if (client_first.substr(0, kGs2Header.size()) != kGs2Header) {
    Dmsg0(kDebugLevel, "scram: bad gs2 header in client-first\n");
    result = HandshakeResult::FORMAT_MISMATCH;
    bs_->fsend("1999 Authorization failed.\n");
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }
  std::string client_first_bare = client_first.substr(kGs2Header.size());

  // Extract client nonce: "n=<name>,r=<cnonce>"
  auto r_pos = client_first_bare.find(",r=");
  if (r_pos == std::string::npos) {
    result = HandshakeResult::FORMAT_MISMATCH;
    bs_->fsend("1999 Authorization failed.\n");
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }
  std::string client_nonce = client_first_bare.substr(r_pos + 3);

  // --- Build and send server-first-message ---
  std::string server_nonce = client_nonce + GenerateNonce();
  std::string salt_b64 = Base64Encode(verifier_.salt);

  // server-first: "r=<nonces>,s=<salt-b64>,i=<N>"
  std::string server_first = "r=" + server_nonce + ",s=" + salt_b64
                             + ",i=" + std::to_string(verifier_.iterations);

  if (!bs_->fsend("%s\n", server_first.c_str())) {
    Dmsg1(kDebugLevel, "scram: send server-first failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }

  // --- Receive client-final-message ---
  if (bs_->WaitData(kAuthTimeoutSecs) <= 0 || bs_->recv() <= 0) {
    Dmsg1(kDebugLevel, "scram: recv client-final failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }
  std::string client_final(bs_->msg);
  // Strip trailing newline if present
  if (!client_final.empty() && client_final.back() == '\n') {
    client_final.pop_back();
  }

  // Split off proof: everything before ",p=" is client-final-without-proof
  auto p_pos = client_final.rfind(",p=");
  if (p_pos == std::string::npos) {
    result = HandshakeResult::FORMAT_MISMATCH;
    bs_->fsend("1999 Authorization failed.\n");
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }
  std::string client_final_no_proof = client_final.substr(0, p_pos);
  std::string client_proof_b64 = client_final.substr(p_pos + 3);

  // Verify the combined nonce in client-final starts with client nonce
  // client-final-without-proof: "c=biws,r=<nonces>"
  if (client_final_no_proof.find("r=" + server_nonce) == std::string::npos) {
    Dmsg0(kDebugLevel, "scram: nonce mismatch in client-final\n");
    result = HandshakeResult::WRONG_HASH;
    bs_->fsend("1999 Authorization failed.\n");
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }

  // AuthMessage = client-first-bare + "," + server-first
  //             + "," + client-final-without-proof
  std::string auth_message
      = client_first_bare + "," + server_first + "," + client_final_no_proof;

  // Verify ClientProof
  // ClientSignature = HMAC(StoredKey, AuthMessage)
  // ClientKey = ClientProof XOR ClientSignature
  // Verify: H(ClientKey) == StoredKey
  std::string client_signature
      = HmacSha256(verifier_.stored_key, auth_message);
  std::string client_proof = Base64Decode(client_proof_b64);
  if (client_proof.size() != kSha256Len) {
    result = HandshakeResult::FORMAT_MISMATCH;
    bs_->fsend("1999 Authorization failed.\n");
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }

  std::string recovered_client_key = client_proof;
  XorBytes(recovered_client_key, client_signature);
  std::string recovered_stored_key = Sha256(recovered_client_key);

  if (recovered_stored_key != verifier_.stored_key) {
    Dmsg0(kDebugLevel, "scram: client proof verification failed\n");
    result = HandshakeResult::WRONG_HASH;
    bs_->fsend("1999 Authorization failed.\n");
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }

  // --- Send server-final-message (mutual auth proof) ---
  std::string server_signature_b64
      = Base64Encode(HmacSha256(verifier_.server_key, auth_message));

  if (!bs_->fsend("v=%s\n", server_signature_b64.c_str())) {
    Dmsg1(kDebugLevel, "scram: send server-final failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }

  bs_->fsend("1000 OK auth\n");
  result = HandshakeResult::SUCCESS;
  return true;
}

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

bool ScramSha256Handshake::ClientSide()
{
  // --- Send client-first-message ---
  std::string client_nonce = GenerateNonce();
  // Use own_qualified_name_ as the username in the SCRAM exchange.
  std::string client_first_bare
      = "n=" + own_qualified_name_ + ",r=" + client_nonce;
  std::string client_first = "n,," + client_first_bare;

  if (!bs_->fsend("%s\n", client_first.c_str())) {
    Dmsg1(kDebugLevel, "scram: send client-first failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }

  // --- Receive server-first-message ---
  if (bs_->WaitData(kAuthTimeoutSecs) <= 0 || bs_->recv() <= 0) {
    Dmsg1(kDebugLevel, "scram: recv server-first failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }
  std::string server_first(bs_->msg);
  if (!server_first.empty() && server_first.back() == '\n') {
    server_first.pop_back();
  }

  // Parse "r=<nonces>,s=<salt-b64>,i=<N>"
  auto parse_attr = [&](const std::string& key) -> std::string {
    std::string prefix = key + "=";
    auto pos = server_first.find(prefix);
    if (pos == std::string::npos) { return {}; }
    pos += prefix.size();
    auto end = server_first.find(',', pos);
    return server_first.substr(pos,
                               end == std::string::npos ? end : end - pos);
  };

  std::string server_nonce = parse_attr("r");
  std::string salt_b64 = parse_attr("s");
  std::string iter_str = parse_attr("i");

  if (server_nonce.empty() || salt_b64.empty() || iter_str.empty()) {
    Dmsg0(kDebugLevel, "scram: bad server-first format\n");
    result = HandshakeResult::FORMAT_MISMATCH;
    return false;
  }

  // Server nonce must start with our client nonce
  if (server_nonce.substr(0, client_nonce.size()) != client_nonce) {
    Dmsg0(kDebugLevel, "scram: server nonce does not start with client nonce\n");
    result = HandshakeResult::WRONG_HASH;
    return false;
  }

  int iterations{};
  try {
    iterations = std::stoi(iter_str);
  } catch (...) {
    result = HandshakeResult::FORMAT_MISMATCH;
    return false;
  }
  if (iterations < 1) {
    result = HandshakeResult::FORMAT_MISMATCH;
    return false;
  }

  std::string salt = Base64Decode(salt_b64);

  // Derive keys from password
  std::string salted_password = Pbkdf2Sha256(password_, salt, iterations);
  std::string client_key = HmacSha256(salted_password, "Client Key");
  std::string stored_key = Sha256(client_key);
  std::string server_key = HmacSha256(salted_password, "Server Key");

  // client-final-without-proof: "c=biws,r=<server_nonce>"
  // "biws" is base64("n,,") — the GS2 header with no channel binding
  static const std::string kChannelBinding = "biws";
  std::string client_final_no_proof
      = "c=" + kChannelBinding + ",r=" + server_nonce;

  // AuthMessage
  std::string auth_message
      = client_first_bare + "," + server_first + "," + client_final_no_proof;

  // Compute ClientProof = ClientKey XOR HMAC(StoredKey, AuthMessage)
  std::string client_signature = HmacSha256(stored_key, auth_message);
  std::string client_proof = client_key;
  XorBytes(client_proof, client_signature);
  std::string client_proof_b64 = Base64Encode(client_proof);

  // --- Send client-final-message ---
  std::string client_final
      = client_final_no_proof + ",p=" + client_proof_b64;
  if (!bs_->fsend("%s\n", client_final.c_str())) {
    Dmsg1(kDebugLevel, "scram: send client-final failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }

  // --- Receive server-final-message ---
  if (bs_->WaitData(kAuthTimeoutSecs) <= 0 || bs_->recv() <= 0) {
    Dmsg1(kDebugLevel, "scram: recv server-final failed: %s\n",
          bs_->bstrerror());
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }
  std::string server_final(bs_->msg);
  if (!server_final.empty() && server_final.back() == '\n') {
    server_final.pop_back();
  }

  // Parse "v=<ServerSignature-b64>"
  if (server_final.substr(0, 2) != "v=") {
    Dmsg1(kDebugLevel, "scram: unexpected server-final: %s\n",
          server_final.c_str());
    result = HandshakeResult::FORMAT_MISMATCH;
    return false;
  }
  std::string received_sig = Base64Decode(server_final.substr(2));

  // Verify server signature (mutual auth)
  std::string expected_sig = HmacSha256(server_key, auth_message);
  if (received_sig != expected_sig) {
    Dmsg0(kDebugLevel, "scram: server signature verification failed\n");
    result = HandshakeResult::WRONG_HASH;
    return false;
  }

  // Receive final OK from server
  if (bs_->WaitData(kAuthTimeoutSecs) <= 0 || bs_->recv() <= 0) {
    result = HandshakeResult::NETWORK_ERROR;
    return false;
  }
  if (!bstrcmp(bs_->msg, "1000 OK auth\n")) {
    Dmsg1(kDebugLevel, "scram: bad final ack: %s\n", bs_->msg);
    result = HandshakeResult::WRONG_HASH;
    Bmicrosleep(bs_->sleep_time_after_authentication_error, 0);
    return false;
  }

  result = HandshakeResult::SUCCESS;
  return true;
}

// ---------------------------------------------------------------------------
// DoHandshake
// ---------------------------------------------------------------------------

bool ScramSha256Handshake::DoHandshake(bool initiated_by_remote)
{
  bool ok = initiated_by_remote ? ServerSide() : ClientSide();
  if (!ok) {
    Dmsg1(kDebugLevel, "scram-sha-256 auth failed with %s\n", bs_->who());
  }
  return ok;
}
