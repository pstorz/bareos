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

#include "gtest/gtest.h"

#include "dird/subscription_manager.h"
#include "lib/base64.h"
#include "lib/subscription_contract.h"
#include "tests/init_openssl.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace {
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpPkeyCtxPtr
    = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

EvpPkeyPtr GenerateEd25519Key()
{
  EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                    EVP_PKEY_CTX_free);
  EXPECT_NE(ctx, nullptr);
  EXPECT_GT(EVP_PKEY_keygen_init(ctx.get()), 0);

  EVP_PKEY* raw_key = nullptr;
  EXPECT_GT(EVP_PKEY_keygen(ctx.get(), &raw_key), 0);
  return EvpPkeyPtr(raw_key, EVP_PKEY_free);
}

std::vector<std::uint8_t> Sign(EVP_PKEY* private_key, std::string_view data)
{
  EvpMdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  EXPECT_NE(ctx, nullptr);
  EXPECT_GT(
      EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, private_key), 0);

  size_t signature_size = 0;
  EXPECT_GT(EVP_DigestSign(ctx.get(), nullptr, &signature_size,
                           reinterpret_cast<const unsigned char*>(data.data()),
                           data.size()),
            0);

  std::vector<std::uint8_t> signature(signature_size);
  EXPECT_GT(EVP_DigestSign(ctx.get(), signature.data(), &signature_size,
                           reinterpret_cast<const unsigned char*>(data.data()),
                           data.size()),
            0);
  signature.resize(signature_size);
  return signature;
}

std::string PublicKeyPem(EVP_PKEY* key)
{
  BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
  EXPECT_NE(bio, nullptr);
  EXPECT_GT(PEM_write_bio_PUBKEY(bio.get(), key), 0);

  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio.get(), &memory);
  EXPECT_NE(memory, nullptr);
  return std::string(memory->data, memory->length);
}

std::string EncodeBase64(const std::vector<std::uint8_t>& data)
{
  std::string out(BASE64_SIZE(data.size()), '\0');
  int written = BinToBase64(
      out.data(), static_cast<int>(out.size()),
      reinterpret_cast<char*>(const_cast<std::uint8_t*>(data.data())),
      static_cast<int>(data.size()), true);
  out.resize(written);
  return out;
}

subscription::SubscriptionContract MakeUnsignedContract()
{
  subscription::SubscriptionContract contract;
  contract.customer_name = "Example Customer GmbH";
  contract.backup_units = 40;
  contract.support
      = subscription::SubscriptionContract::Support{"Standard", true};
  contract.expiration_date = {2027, 12, 31};
  contract.key_id = "main-2026";
  return contract;
}

std::string RenderFile(const subscription::SubscriptionContract& contract,
                       std::string_view signature_base64)
{
  std::string expiration_date
      = subscription::FormatSubscriptionContractExpirationDate(
          contract.expiration_date);
  return "{\n"
         "  \"format_version\": 1,\n"
         "  \"customer_name\": \"" + contract.customer_name
         + "\",\n"
           "  \"backup_units\": "
         + std::to_string(contract.backup_units)
         + ",\n"
           "  \"support\": {\n"
           "    \"level\": \""
         + contract.support->level
         + "\",\n"
           "    \"rear_support\": "
         + (contract.support->rear_support ? "true" : "false")
         + "\n"
           "  },\n"
           "  \"expiration_date\": \""
         + expiration_date
         + "\",\n"
           "  \"key_id\": \""
         + contract.key_id
         + "\",\n"
           "  \"signature\": \""
         + std::string(signature_base64) + "\"\n}";
}

class TempDir {
 public:
  TempDir()
  {
    char pattern[] = "/tmp/subscription-manager-XXXXXX";
    char* created = mkdtemp(pattern);
    EXPECT_NE(created, nullptr);
    path_ = created;
  }

  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, std::string_view content)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(file.is_open());
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  ASSERT_TRUE(file.good());
}

class SubscriptionManagerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { InitOpenSsl(); }
};
}  // namespace

TEST_F(SubscriptionManagerTest, MissingImplicitDefaultIsNotConfigured)
{
  TempDir temp_dir;
  auto default_path = (temp_dir.path() / "subscription.json").string();
  directordaemon::SubscriptionContractManager manager(nullptr, 0, default_path);

  auto snapshot = manager.Reload("");

  EXPECT_EQ(snapshot.load_state,
            directordaemon::SubscriptionContractLoadState::kNotConfigured);
  EXPECT_FALSE(snapshot.explicitly_configured);
  EXPECT_EQ(snapshot.file_path, default_path);
}

TEST_F(SubscriptionManagerTest, MissingExplicitFileWarnStateIsFileMissing)
{
  TempDir temp_dir;
  directordaemon::SubscriptionContractManager manager(
      nullptr, 0, (temp_dir.path() / "default.json").string());
  auto configured = (temp_dir.path() / "configured.json").string();

  auto snapshot = manager.Reload(configured);

  EXPECT_EQ(snapshot.load_state,
            directordaemon::SubscriptionContractLoadState::kFileMissing);
  EXPECT_TRUE(snapshot.explicitly_configured);
  EXPECT_EQ(snapshot.file_path, configured);
}

TEST_F(SubscriptionManagerTest, ValidContractLoadsWithTrustedCompiledKey)
{
  TempDir temp_dir;
  auto key = GenerateEd25519Key();
  auto public_key = PublicKeyPem(key.get());
  directordaemon::SubscriptionTrustedPublicKey trusted_key{"main-2026",
                                                           public_key.c_str()};
  directordaemon::SubscriptionContractManager manager(
      &trusted_key, 1, (temp_dir.path() / "default.json").string());

  auto contract = MakeUnsignedContract();
  auto signature = Sign(
      key.get(), subscription::CanonicalizeSubscriptionContract(contract));
  auto file_path = temp_dir.path() / "subscription.json";
  WriteFile(file_path, RenderFile(contract, EncodeBase64(signature)));

  auto snapshot = manager.Reload(file_path.string());

  ASSERT_EQ(snapshot.load_state,
            directordaemon::SubscriptionContractLoadState::kValid);
  ASSERT_TRUE(snapshot.contract.has_value());
  ASSERT_TRUE(snapshot.validity.has_value());
  EXPECT_EQ(snapshot.contract->customer_name, contract.customer_name);
  EXPECT_EQ(snapshot.contract->backup_units, contract.backup_units);
}

TEST_F(SubscriptionManagerTest, UnknownKeyIdIsRejectedBeforeVerification)
{
  TempDir temp_dir;
  auto key = GenerateEd25519Key();
  auto public_key = PublicKeyPem(key.get());
  directordaemon::SubscriptionTrustedPublicKey trusted_key{"different-key",
                                                           public_key.c_str()};
  directordaemon::SubscriptionContractManager manager(
      &trusted_key, 1, (temp_dir.path() / "default.json").string());

  auto contract = MakeUnsignedContract();
  auto signature = Sign(
      key.get(), subscription::CanonicalizeSubscriptionContract(contract));
  auto file_path = temp_dir.path() / "subscription.json";
  WriteFile(file_path, RenderFile(contract, EncodeBase64(signature)));

  auto snapshot = manager.Reload(file_path.string());

  EXPECT_EQ(snapshot.load_state,
            directordaemon::SubscriptionContractLoadState::kKeyUnknown);
  EXPECT_EQ(snapshot.detail, "main-2026");
  EXPECT_FALSE(snapshot.contract.has_value());
}

TEST_F(SubscriptionManagerTest, SignatureMismatchIsRejected)
{
  TempDir temp_dir;
  auto signing_key = GenerateEd25519Key();
  auto trusted_verifier_key = GenerateEd25519Key();
  auto trusted_public_key = PublicKeyPem(trusted_verifier_key.get());
  directordaemon::SubscriptionTrustedPublicKey trusted_key{
      "main-2026", trusted_public_key.c_str()};
  directordaemon::SubscriptionContractManager manager(
      &trusted_key, 1, (temp_dir.path() / "default.json").string());

  auto contract = MakeUnsignedContract();
  auto signature
      = Sign(signing_key.get(),
             subscription::CanonicalizeSubscriptionContract(contract));
  auto file_path = temp_dir.path() / "subscription.json";
  WriteFile(file_path, RenderFile(contract, EncodeBase64(signature)));

  auto snapshot = manager.Reload(file_path.string());

  EXPECT_EQ(snapshot.load_state,
            directordaemon::SubscriptionContractLoadState::kSignatureInvalid);
  EXPECT_EQ(snapshot.detail, "signature mismatch");
}

TEST_F(SubscriptionManagerTest, RefreshReplacesCachedValidStateWhenFileTurnsBad)
{
  TempDir temp_dir;
  auto key = GenerateEd25519Key();
  auto public_key = PublicKeyPem(key.get());
  directordaemon::SubscriptionTrustedPublicKey trusted_key{"main-2026",
                                                           public_key.c_str()};
  directordaemon::SubscriptionContractManager manager(
      &trusted_key, 1, (temp_dir.path() / "default.json").string());

  auto contract = MakeUnsignedContract();
  auto signature = Sign(
      key.get(), subscription::CanonicalizeSubscriptionContract(contract));
  auto file_path = temp_dir.path() / "subscription.json";
  WriteFile(file_path, RenderFile(contract, EncodeBase64(signature)));

  auto loaded = manager.Reload(file_path.string());
  ASSERT_EQ(loaded.load_state,
            directordaemon::SubscriptionContractLoadState::kValid);

  WriteFile(file_path, "{\"format_version\": 1, \"broken\": true");

  auto refreshed = manager.RefreshIfChanged(file_path.string());

  EXPECT_EQ(refreshed.load_state,
            directordaemon::SubscriptionContractLoadState::kParseError);
  EXPECT_FALSE(refreshed.contract.has_value());
}

TEST_F(SubscriptionManagerTest,
       RefreshUpdatesValidityWhenFileContentIsUnchanged)
{
  TempDir temp_dir;
  auto key = GenerateEd25519Key();
  auto public_key = PublicKeyPem(key.get());
  directordaemon::SubscriptionTrustedPublicKey trusted_key{"main-2026",
                                                           public_key.c_str()};
  directordaemon::SubscriptionContractManager manager(
      &trusted_key, 1, (temp_dir.path() / "default.json").string());

  auto contract = MakeUnsignedContract();
  contract.expiration_date = {2026, 1, 15};
  auto signature = Sign(
      key.get(), subscription::CanonicalizeSubscriptionContract(contract));
  auto file_path = temp_dir.path() / "subscription.json";
  WriteFile(file_path, RenderFile(contract, EncodeBase64(signature)));

  auto initially_valid = manager.Reload(file_path.string(), 1761955200);
  ASSERT_EQ(initially_valid.load_state,
            directordaemon::SubscriptionContractLoadState::kValid);
  ASSERT_EQ(*initially_valid.validity, subscription::ContractValidity::kValid);

  auto expiring = manager.RefreshIfChanged(file_path.string(), 1763596800);
  ASSERT_EQ(expiring.load_state,
            directordaemon::SubscriptionContractLoadState::kValid);
  ASSERT_EQ(*expiring.validity, subscription::ContractValidity::kExpiringSoon);

  auto expired = manager.RefreshIfChanged(file_path.string(), 1768521600);
  ASSERT_EQ(expired.load_state,
            directordaemon::SubscriptionContractLoadState::kValid);
  ASSERT_EQ(*expired.validity, subscription::ContractValidity::kExpired);
}

TEST_F(SubscriptionManagerTest, WarningCallbackOnlyRunsOnStateTransitions)
{
  TempDir temp_dir;
  std::vector<std::string> warnings;
  auto callback
      = [&warnings](
            const directordaemon::SubscriptionContractSnapshot& snapshot) {
          warnings.push_back(snapshot.file_path + ":" + snapshot.detail);
        };

  directordaemon::SubscriptionContractManager manager(
      nullptr, 0, (temp_dir.path() / "default.json").string(), callback);
  auto configured = (temp_dir.path() / "missing.json").string();

  auto first = manager.Reload(configured);
  auto second = manager.RefreshIfChanged(configured);

  EXPECT_EQ(first.load_state,
            directordaemon::SubscriptionContractLoadState::kFileMissing);
  EXPECT_EQ(second.load_state,
            directordaemon::SubscriptionContractLoadState::kFileMissing);
  ASSERT_EQ(warnings.size(), 1U);
}
