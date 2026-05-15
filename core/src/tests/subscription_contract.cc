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

#include <jansson.h>
#include "lib/base64.h"
#include "lib/subscription_contract.h"
#include "tests/init_openssl.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpPkeyCtxPtr
    = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
struct JsonDeleter {
  void operator()(json_t* value) const
  {
    if (value) { json_decref(value); }
  }
};
using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

std::int64_t DaysFromCivil(const subscription::CivilDate& date)
{
  int year = date.year;
  unsigned month = static_cast<unsigned>(date.month);
  unsigned day = static_cast<unsigned>(date.day);

  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy
      = (153 * (month + (month > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5
        + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097
         + static_cast<std::int64_t>(doe) - 719468;
}

std::time_t ToUnixTime(const subscription::CivilDate& date)
{
  return static_cast<std::time_t>(DaysFromCivil(date) * 86400);
}

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
  JsonPtr json{json_object()};
  json_object_set_new(json.get(), "format_version",
                      json_integer(contract.format_version));
  json_object_set_new(json.get(), "customer_name",
                      json_stringn(contract.customer_name.data(),
                                   contract.customer_name.size()));
  json_object_set_new(json.get(), "backup_units",
                      json_integer(contract.backup_units));
  if (contract.support) {
    JsonPtr support{json_object()};
    json_object_set_new(support.get(), "level",
                        json_stringn(contract.support->level.data(),
                                     contract.support->level.size()));
    json_object_set_new(support.get(), "rear_support",
                        json_boolean(contract.support->rear_support));
    json_object_set_new(json.get(), "support", support.release());
  } else {
    json_object_set_new(json.get(), "support", json_null());
  }
  auto expiration_date = subscription::FormatSubscriptionContractExpirationDate(
      contract.expiration_date);
  json_object_set_new(
      json.get(), "expiration_date",
      json_stringn(expiration_date.data(), expiration_date.size()));
  json_object_set_new(
      json.get(), "key_id",
      json_stringn(contract.key_id.data(), contract.key_id.size()));
  json_object_set_new(json.get(), "x_future_field",
                      json_string("ignored-by-v1"));
  json_object_set_new(
      json.get(), "signature",
      json_stringn(signature_base64.data(), signature_base64.size()));

  char* dumped = json_dumps(json.get(), JSON_INDENT(2) | JSON_SORT_KEYS);
  EXPECT_NE(dumped, nullptr);
  std::string result(dumped);
  free(dumped);
  return result;
}

class SubscriptionContractTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { InitOpenSsl(); }
};
}  // namespace

TEST_F(SubscriptionContractTest, ParseCanonicalizesRecognizedFields)
{
  auto contract = MakeUnsignedContract();
  std::vector<std::uint8_t> raw_signature(subscription::kEd25519SignatureSize,
                                          0x5a);
  auto file = RenderFile(contract, EncodeBase64(raw_signature));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_FALSE(parsed.holds_error()) << parsed.error_unchecked().c_str();

  EXPECT_EQ(parsed.value_unchecked().format_version,
            subscription::kCurrentSubscriptionContractFormatVersion);
  EXPECT_EQ(parsed.value_unchecked().customer_name, contract.customer_name);
  EXPECT_EQ(parsed.value_unchecked().backup_units, contract.backup_units);
  ASSERT_TRUE(parsed.value_unchecked().support.has_value());
  EXPECT_EQ(parsed.value_unchecked().support->level, contract.support->level);
  EXPECT_EQ(parsed.value_unchecked().support->rear_support,
            contract.support->rear_support);
  EXPECT_EQ(parsed.value_unchecked().expiration_date, contract.expiration_date);
  EXPECT_EQ(parsed.value_unchecked().key_id, contract.key_id);
  EXPECT_EQ(parsed.value_unchecked().signature, raw_signature);

  EXPECT_EQ(
      subscription::CanonicalizeSubscriptionContract(parsed.value_unchecked()),
      "{\"backup_units\":40,\"customer_name\":\"Example Customer GmbH\","
      "\"expiration_date\":\"2027-12-31\",\"format_version\":1,"
      "\"key_id\":\"main-2026\",\"support\":{\"level\":\"Standard\","
      "\"rear_support\":true}}");
}

TEST_F(SubscriptionContractTest, MissingSupportCanonicalizesAsNull)
{
  auto contract = MakeUnsignedContract();
  contract.support.reset();
  auto file
      = RenderFile(contract, EncodeBase64(std::vector<std::uint8_t>(
                                 subscription::kEd25519SignatureSize, 0x11)));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_FALSE(parsed.holds_error()) << parsed.error_unchecked().c_str();

  EXPECT_FALSE(parsed.value_unchecked().support.has_value());
  EXPECT_EQ(
      subscription::CanonicalizeSubscriptionContract(parsed.value_unchecked()),
      "{\"backup_units\":40,\"customer_name\":\"Example Customer GmbH\","
      "\"expiration_date\":\"2027-12-31\",\"format_version\":1,"
      "\"key_id\":\"main-2026\",\"support\":null}");
}

TEST_F(SubscriptionContractTest, RejectsDuplicateRecognizedField)
{
  auto file = R"({
  "format_version": 1,
  "customer_name": "Example Customer GmbH",
  "backup_units": 40,
  "backup_units": 99,
  "expiration_date": "2027-12-31",
  "key_id": "main-2026",
  "signature": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="
})";

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_TRUE(parsed.holds_error());
  EXPECT_NE(std::string(parsed.error_unchecked().c_str()).find("duplicate"),
            std::string::npos);
}

TEST_F(SubscriptionContractTest, RejectsInvalidUtf8CustomerName)
{
  auto encoded = EncodeBase64(
      std::vector<std::uint8_t>(subscription::kEd25519SignatureSize, 0x33));
  std::string invalid_name("Broken ");
  invalid_name.push_back(static_cast<char>(0xc3));
  invalid_name.push_back(static_cast<char>(0x28));

  std::string file = "{\"format_version\":1,\"customer_name\":\"";
  file += invalid_name;
  file += "\",\"backup_units\":40,\"expiration_date\":\"2027-12-31\","
          "\"key_id\":\"main-2026\",\"signature\":\"";
  file += encoded;
  file += "\"}";

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_TRUE(parsed.holds_error());
  EXPECT_NE(std::string(parsed.error_unchecked().c_str())
                .find("failed to parse subscription contract JSON"),
            std::string::npos);
}

TEST_F(SubscriptionContractTest, RejectsUnsupportedFormatVersion)
{
  auto contract = MakeUnsignedContract();
  contract.format_version = 2;
  auto file
      = RenderFile(contract, EncodeBase64(std::vector<std::uint8_t>(
                                 subscription::kEd25519SignatureSize, 0x44)));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_TRUE(parsed.holds_error());
  EXPECT_NE(std::string(parsed.error_unchecked().c_str())
                .find("format_version 2 is unsupported"),
            std::string::npos);
}

TEST_F(SubscriptionContractTest, VerifiesEd25519SignatureRoundTrip)
{
  auto key = GenerateEd25519Key();
  auto contract = MakeUnsignedContract();
  contract.signature = Sign(
      key.get(), subscription::CanonicalizeSubscriptionContract(contract));
  auto file = RenderFile(contract, EncodeBase64(contract.signature));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_FALSE(parsed.holds_error()) << parsed.error_unchecked().c_str();

  auto verified = subscription::VerifySubscriptionContractSignature(
      parsed.value_unchecked(), PublicKeyPem(key.get()));
  ASSERT_FALSE(verified.holds_error()) << verified.error_unchecked().c_str();
  EXPECT_TRUE(verified.value_unchecked());
}

TEST_F(SubscriptionContractTest, DetectsSignatureMismatch)
{
  auto key = GenerateEd25519Key();
  auto contract = MakeUnsignedContract();
  contract.signature = Sign(
      key.get(), subscription::CanonicalizeSubscriptionContract(contract));
  auto file = RenderFile(contract, EncodeBase64(contract.signature));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_FALSE(parsed.holds_error()) << parsed.error_unchecked().c_str();
  parsed.value_unchecked().backup_units += 1;

  auto verified = subscription::VerifySubscriptionContractSignature(
      parsed.value_unchecked(), PublicKeyPem(key.get()));
  ASSERT_FALSE(verified.holds_error()) << verified.error_unchecked().c_str();
  EXPECT_FALSE(verified.value_unchecked());
}

TEST_F(SubscriptionContractTest, EvaluatesExpirationStates)
{
  subscription::CivilDate expiration{2027, 5, 15};

  EXPECT_EQ(subscription::EvaluateSubscriptionContractValidity(
                expiration, ToUnixTime({2027, 3, 15})),
            subscription::ContractValidity::kValid);
  EXPECT_EQ(subscription::EvaluateSubscriptionContractValidity(
                expiration, ToUnixTime({2027, 4, 1})),
            subscription::ContractValidity::kExpiringSoon);
  EXPECT_EQ(subscription::EvaluateSubscriptionContractValidity(
                expiration, ToUnixTime({2027, 5, 16})),
            subscription::ContractValidity::kExpired);
}

TEST_F(SubscriptionContractTest, AcceptsUnknownSupportLevelName)
{
  auto contract = MakeUnsignedContract();
  contract.support
      = subscription::SubscriptionContract::Support{"Partner Gold Plus", false};
  auto file
      = RenderFile(contract, EncodeBase64(std::vector<std::uint8_t>(
                                 subscription::kEd25519SignatureSize, 0x55)));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_FALSE(parsed.holds_error()) << parsed.error_unchecked().c_str();
  ASSERT_TRUE(parsed.value_unchecked().support.has_value());
  EXPECT_EQ(parsed.value_unchecked().support->level, "Partner Gold Plus");
  EXPECT_FALSE(parsed.value_unchecked().support->rear_support);
}

TEST_F(SubscriptionContractTest, RejectsBackupUnitsOutsidePricelistScale)
{
  auto contract = MakeUnsignedContract();
  contract.backup_units = 25;
  auto file
      = RenderFile(contract, EncodeBase64(std::vector<std::uint8_t>(
                                 subscription::kEd25519SignatureSize, 0x66)));

  auto parsed = subscription::ParseSubscriptionContract(file);
  ASSERT_TRUE(parsed.holds_error());
  EXPECT_NE(
      std::string(parsed.error_unchecked().c_str()).find("multiple of 10"),
      std::string::npos);
}

TEST_F(SubscriptionContractTest, RejectsOversizedContractFiles)
{
  std::string oversized(subscription::kMaxSubscriptionContractFileSize + 1,
                        'a');

  auto parsed = subscription::ParseSubscriptionContract(oversized);
  ASSERT_TRUE(parsed.holds_error());
  EXPECT_NE(std::string(parsed.error_unchecked().c_str())
                .find("maximum supported size"),
            std::string::npos);
}
