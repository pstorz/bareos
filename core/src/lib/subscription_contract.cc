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

#include "lib/subscription_contract.h"

#include <jansson.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <memory>
#include <utility>

#include "include/bareos.h"
#include "lib/base64.h"

#include <utf8.h>

namespace subscription {
namespace {
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
struct JsonDeleter {
  void operator()(json_t* value) const
  {
    if (value) { json_decref(value); }
  }
};
using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

template <typename T, typename... Args>
result<T> ErrorResult(const char* fmt, Args... args)
{
  PoolMem error(PM_MESSAGE);
  if constexpr (sizeof...(Args) == 0) {
    PmStrcpy(error, fmt);
  } else {
    Mmsg(error, fmt, args...);
  }
  return result<T>{std::move(error)};
}

bool ContainsControlCharacters(std::string_view input)
{
  return std::any_of(input.begin(), input.end(), [](char c) {
    return std::iscntrl(static_cast<unsigned char>(c)) != 0;
  });
}

bool IsValidUtf8Text(std::string_view input)
{
  return !input.empty() && !ContainsControlCharacters(input)
         && utf8::is_valid(input.begin(), input.end());
}

bool IsValidKeyId(std::string_view input)
{
  if (input.empty()) { return false; }

  return std::all_of(input.begin(), input.end(), [](char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '.' || c == '_' || c == '-';
  });
}

bool IsLeapYear(int year)
{
  return ((year % 4) == 0 && (year % 100) != 0) || (year % 400) == 0;
}

int DaysInMonth(int year, int month)
{
  static constexpr std::array<int, 12> kDaysPerMonth
      = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (month == 2 && IsLeapYear(year)) { return 29; }

  return kDaysPerMonth.at(month - 1);
}

result<uint64_t> ParseUnsignedInteger(std::string_view input, const char* field)
{
  if (input.empty()) {
    return ErrorResult<uint64_t>("%s must not be empty", field);
  }

  if (!std::all_of(input.begin(), input.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    return ErrorResult<uint64_t>("%s must contain only decimal digits", field);
  }

  uint64_t value{};
  for (char c : input) {
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > ((std::numeric_limits<uint64_t>::max() - digit) / 10)) {
      return ErrorResult<uint64_t>("%s is out of range", field);
    }
    value = value * 10 + digit;
  }

  return result<uint64_t>{value};
}

result<CivilDate> ParseCivilDate(std::string_view input)
{
  if (input.size() != 10 || input[4] != '-' || input[7] != '-') {
    return ErrorResult<CivilDate>(
        "expiration_date must use the YYYY-MM-DD format");
  }

  auto year_result
      = ParseUnsignedInteger(input.substr(0, 4), "expiration_date");
  if (year_result.holds_error()) {
    return ErrorResult<CivilDate>("%s", year_result.error_unchecked().c_str());
  }

  auto month_result
      = ParseUnsignedInteger(input.substr(5, 2), "expiration_date");
  if (month_result.holds_error()) {
    return ErrorResult<CivilDate>("%s", month_result.error_unchecked().c_str());
  }

  auto day_result = ParseUnsignedInteger(input.substr(8, 2), "expiration_date");
  if (day_result.holds_error()) {
    return ErrorResult<CivilDate>("%s", day_result.error_unchecked().c_str());
  }

  CivilDate parsed{static_cast<int>(*year_result.value()),
                   static_cast<int>(*month_result.value()),
                   static_cast<int>(*day_result.value())};

  if (parsed.month < 1 || parsed.month > 12) {
    return ErrorResult<CivilDate>(
        "expiration_date month must be in the range 1-12");
  }

  if (parsed.day < 1 || parsed.day > DaysInMonth(parsed.year, parsed.month)) {
    return ErrorResult<CivilDate>("expiration_date day is out of range");
  }

  return result<CivilDate>{parsed};
}

std::int64_t DaysFromCivil(const CivilDate& date)
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

std::int64_t FloorDivide(std::int64_t value, std::int64_t divisor)
{
  std::int64_t quotient = value / divisor;
  std::int64_t remainder = value % divisor;
  if (remainder != 0 && ((remainder < 0) != (divisor < 0))) { --quotient; }
  return quotient;
}

std::string OpenSslErrorString()
{
  unsigned long error = ERR_peek_last_error();
  if (error == 0) { return "unknown OpenSSL error"; }

  char buffer[256];
  ERR_error_string_n(error, buffer, sizeof(buffer));
  return buffer;
}

result<std::vector<std::uint8_t>> DecodeBase64Signature(std::string_view input)
{
  if (input.empty()) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "signature must not be empty");
  }

  std::string base64_value(input);
  std::vector<std::uint8_t> decoded(((base64_value.size() + 3) / 4) * 3);

  int decoded_size = Base64ToBin(
      reinterpret_cast<char*>(decoded.data()), static_cast<int>(decoded.size()),
      base64_value.data(), static_cast<int>(base64_value.size()));
  if (decoded_size <= 0) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "signature is not valid base64 data");
  }

  decoded.resize(decoded_size);

  if (decoded.size() != kEd25519SignatureSize) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "signature must decode to exactly %d bytes",
        static_cast<int>(kEd25519SignatureSize));
  }

  return result<std::vector<std::uint8_t>>{std::move(decoded)};
}

result<std::string> EncodeBase64Signature(
    const std::vector<std::uint8_t>& signature)
{
  if (signature.size() != kEd25519SignatureSize) {
    return ErrorResult<std::string>("signature must contain exactly %d bytes",
                                    static_cast<int>(kEd25519SignatureSize));
  }

  std::string encoded(BASE64_SIZE(signature.size()), '\0');
  int written = BinToBase64(
      encoded.data(), static_cast<int>(encoded.size()),
      reinterpret_cast<char*>(const_cast<std::uint8_t*>(signature.data())),
      static_cast<int>(signature.size()), true);
  if (written <= 0) {
    return ErrorResult<std::string>("failed to base64 encode signature");
  }

  encoded.resize(static_cast<std::size_t>(written));
  return result<std::string>{std::move(encoded)};
}

result<std::string_view> GetRequiredJsonString(json_t* root, const char* key)
{
  json_t* value = json_object_get(root, key);
  if (!value) {
    return ErrorResult<std::string_view>(
        "subscription contract is missing required key '%s'", key);
  }
  if (!json_is_string(value)) {
    return ErrorResult<std::string_view>(
        "subscription contract key '%s' must be a JSON string", key);
  }

  const char* string_value = json_string_value(value);
  if (!string_value) {
    return ErrorResult<std::string_view>(
        "subscription contract key '%s' must be a JSON string", key);
  }

  return result<std::string_view>{std::string_view{string_value}};
}

result<uint64_t> GetRequiredJsonUnsignedInteger(json_t* root, const char* key)
{
  json_t* value = json_object_get(root, key);
  if (!value) {
    return ErrorResult<uint64_t>(
        "subscription contract is missing required key '%s'", key);
  }
  if (!json_is_integer(value)) {
    return ErrorResult<uint64_t>(
        "subscription contract key '%s' must be a JSON integer", key);
  }

  json_int_t integer_value = json_integer_value(value);
  if (integer_value < 0) {
    return ErrorResult<uint64_t>(
        "subscription contract key '%s' must not be negative", key);
  }

  return result<uint64_t>{static_cast<uint64_t>(integer_value)};
}

result<std::string> DumpCanonicalJson(json_t* root)
{
  char* dumped = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT);
  if (!dumped) {
    return ErrorResult<std::string>(
        "failed to serialize subscription contract canonical JSON");
  }

  std::string canonical(dumped);
  free(dumped);
  return result<std::string>{std::move(canonical)};
}

result<std::optional<SubscriptionContract::Support>> ParseSupport(json_t* root)
{
  json_t* support = json_object_get(root, "support");
  if (!support || json_is_null(support)) {
    return result<std::optional<SubscriptionContract::Support>>{
        std::optional<SubscriptionContract::Support>{}};
  }
  if (!json_is_object(support)) {
    return ErrorResult<std::optional<SubscriptionContract::Support>>(
        "subscription contract key 'support' must be a JSON object or null");
  }

  auto level_result = GetRequiredJsonString(support, "level");
  if (level_result.holds_error()) {
    return ErrorResult<std::optional<SubscriptionContract::Support>>(
        "%s", level_result.error_unchecked().c_str());
  }
  if (!IsValidUtf8Text(*level_result.value())) {
    return ErrorResult<std::optional<SubscriptionContract::Support>>(
        "support.level must be valid UTF-8 without control characters");
  }

  json_t* rear_support = json_object_get(support, "rear_support");
  bool has_rear_support = false;
  if (rear_support) {
    if (!json_is_boolean(rear_support)) {
      return ErrorResult<std::optional<SubscriptionContract::Support>>(
          "subscription contract key 'support.rear_support' must be a JSON "
          "boolean");
    }
    has_rear_support = json_boolean_value(rear_support) != 0;
  }

  SubscriptionContract::Support parsed{std::string(*level_result.value()),
                                       has_rear_support};
  return result<std::optional<SubscriptionContract::Support>>{
      std::optional<SubscriptionContract::Support>{std::move(parsed)}};
}

enum class SignatureRequirement
{
  kRequired,
  kOptional
};

result<SubscriptionContract> ParseSubscriptionContractImpl(
    std::string_view input,
    SignatureRequirement signature_requirement)
{
  if (input.empty()) {
    return ErrorResult<SubscriptionContract>("subscription contract is empty");
  }

  if (input.size() > kMaxSubscriptionContractFileSize) {
    return ErrorResult<SubscriptionContract>(
        "subscription contract exceeds the maximum supported size of %d bytes",
        static_cast<int>(kMaxSubscriptionContractFileSize));
  }

  json_error_t json_error{};
  JsonPtr root{json_loadb(input.data(), input.size(), JSON_REJECT_DUPLICATES,
                          &json_error)};
  if (!root) {
    return ErrorResult<SubscriptionContract>(
        "failed to parse subscription contract JSON: %s (line %d, column %d)",
        json_error.text, json_error.line, json_error.column);
  }

  if (!json_is_object(root.get())) {
    return ErrorResult<SubscriptionContract>(
        "subscription contract root must be a JSON object");
  }

  auto format_version_result
      = GetRequiredJsonUnsignedInteger(root.get(), "format_version");
  if (format_version_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", format_version_result.error_unchecked().c_str());
  }

  if (*format_version_result.value()
      != kCurrentSubscriptionContractFormatVersion) {
    return ErrorResult<SubscriptionContract>(
        "subscription contract format_version %llu is unsupported",
        static_cast<unsigned long long>(*format_version_result.value()));
  }

  SubscriptionContract contract;
  contract.format_version = *format_version_result.value();

  auto customer_name_result
      = GetRequiredJsonString(root.get(), "customer_name");
  if (customer_name_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", customer_name_result.error_unchecked().c_str());
  }
  if (!IsValidUtf8Text(*customer_name_result.value())) {
    return ErrorResult<SubscriptionContract>(
        "customer_name must be valid UTF-8 without control characters");
  }
  contract.customer_name = std::string(*customer_name_result.value());

  auto backup_units_result
      = GetRequiredJsonUnsignedInteger(root.get(), "backup_units");
  if (backup_units_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", backup_units_result.error_unchecked().c_str());
  }
  if (*backup_units_result.value() < 10) {
    return ErrorResult<SubscriptionContract>(
        "backup_units must be at least 10");
  }
  if ((*backup_units_result.value() % 10) != 0) {
    return ErrorResult<SubscriptionContract>(
        "backup_units must be a multiple of 10");
  }
  contract.backup_units = *backup_units_result.value();

  auto support_result = ParseSupport(root.get());
  if (support_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", support_result.error_unchecked().c_str());
  }
  contract.support = std::move(*support_result.value());

  auto expiration_date_result
      = GetRequiredJsonString(root.get(), "expiration_date");
  if (expiration_date_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", expiration_date_result.error_unchecked().c_str());
  }
  auto parsed_date = ParseCivilDate(*expiration_date_result.value());
  if (parsed_date.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", parsed_date.error_unchecked().c_str());
  }
  contract.expiration_date = *parsed_date.value();

  auto key_id_result = GetRequiredJsonString(root.get(), "key_id");
  if (key_id_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", key_id_result.error_unchecked().c_str());
  }
  if (!IsValidKeyId(*key_id_result.value())) {
    return ErrorResult<SubscriptionContract>(
        "key_id may only contain ASCII letters, digits, '.', '_' or '-'");
  }
  contract.key_id = std::string(*key_id_result.value());

  if (signature_requirement == SignatureRequirement::kRequired) {
    auto signature_result = GetRequiredJsonString(root.get(), "signature");
    if (signature_result.holds_error()) {
      return ErrorResult<SubscriptionContract>(
          "%s", signature_result.error_unchecked().c_str());
    }
    auto decoded_signature = DecodeBase64Signature(*signature_result.value());
    if (decoded_signature.holds_error()) {
      return ErrorResult<SubscriptionContract>(
          "%s", decoded_signature.error_unchecked().c_str());
    }
    contract.signature = std::move(decoded_signature.value_unchecked());
  }

  return result<SubscriptionContract>{std::move(contract)};
}
}  // namespace

result<SubscriptionContract> ParseSubscriptionContract(std::string_view input)
{
  return ParseSubscriptionContractImpl(input, SignatureRequirement::kRequired);
}

result<SubscriptionContract> ParseSubscriptionContractForSigning(
    std::string_view input)
{
  return ParseSubscriptionContractImpl(input, SignatureRequirement::kOptional);
}

std::string FormatSubscriptionContractExpirationDate(const CivilDate& date)
{
  char buffer[11];
  Bsnprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month,
            date.day);
  return buffer;
}

result<std::string> SerializeSubscriptionContract(
    const SubscriptionContract& contract,
    bool compact)
{
  auto signature = EncodeBase64Signature(contract.signature);
  if (signature.holds_error()) {
    return ErrorResult<std::string>("%s", signature.error_unchecked().c_str());
  }

  JsonPtr document{json_object()};
  if (!document) {
    return ErrorResult<std::string>(
        "failed to allocate subscription contract JSON object");
  }

  json_object_set_new(document.get(), "format_version",
                      json_integer(contract.format_version));
  json_object_set_new(document.get(), "customer_name",
                      json_stringn(contract.customer_name.data(),
                                   contract.customer_name.size()));
  json_object_set_new(document.get(), "backup_units",
                      json_integer(contract.backup_units));
  if (contract.support) {
    json_t* support = json_object();
    if (!support) {
      return ErrorResult<std::string>(
          "failed to allocate subscription support JSON object");
    }
    json_object_set_new(support, "level",
                        json_stringn(contract.support->level.data(),
                                     contract.support->level.size()));
    json_object_set_new(support, "rear_support",
                        json_boolean(contract.support->rear_support));
    json_object_set_new(document.get(), "support", support);
  } else {
    json_object_set_new(document.get(), "support", json_null());
  }
  auto expiration_date
      = FormatSubscriptionContractExpirationDate(contract.expiration_date);
  json_object_set_new(
      document.get(), "expiration_date",
      json_stringn(expiration_date.data(), expiration_date.size()));
  json_object_set_new(
      document.get(), "key_id",
      json_stringn(contract.key_id.data(), contract.key_id.size()));
  json_object_set_new(document.get(), "signature",
                      json_stringn(signature.value_unchecked().data(),
                                   signature.value_unchecked().size()));

  char* dumped
      = json_dumps(document.get(),
                   JSON_SORT_KEYS | (compact ? JSON_COMPACT : JSON_INDENT(2)));
  if (!dumped) {
    return ErrorResult<std::string>(
        "failed to serialize subscription contract JSON");
  }

  std::string serialized(dumped);
  free(dumped);
  return result<std::string>{std::move(serialized)};
}

std::string CanonicalizeSubscriptionContract(
    const SubscriptionContract& contract)
{
  JsonPtr canonical{json_object()};
  if (!canonical) { return {}; }

  json_object_set_new(canonical.get(), "format_version",
                      json_integer(contract.format_version));
  json_object_set_new(canonical.get(), "customer_name",
                      json_stringn(contract.customer_name.data(),
                                   contract.customer_name.size()));
  json_object_set_new(canonical.get(), "backup_units",
                      json_integer(contract.backup_units));
  if (contract.support) {
    json_t* support = json_object();
    if (!support) { return {}; }
    json_object_set_new(support, "level",
                        json_stringn(contract.support->level.data(),
                                     contract.support->level.size()));
    json_object_set_new(support, "rear_support",
                        json_boolean(contract.support->rear_support));
    json_object_set_new(canonical.get(), "support", support);
  } else {
    json_object_set_new(canonical.get(), "support", json_null());
  }
  auto expiration_date
      = FormatSubscriptionContractExpirationDate(contract.expiration_date);
  json_object_set_new(
      canonical.get(), "expiration_date",
      json_stringn(expiration_date.data(), expiration_date.size()));
  json_object_set_new(
      canonical.get(), "key_id",
      json_stringn(contract.key_id.data(), contract.key_id.size()));

  auto dumped = DumpCanonicalJson(canonical.get());
  if (dumped.holds_error()) { return {}; }
  return dumped.value_unchecked();
}

result<bool> VerifySubscriptionContractSignature(
    const SubscriptionContract& contract,
    std::string_view public_key_pem)
{
  if (public_key_pem.empty()) {
    return ErrorResult<bool>("public key PEM must not be empty");
  }

  BioPtr bio(BIO_new_mem_buf(public_key_pem.data(),
                             static_cast<int>(public_key_pem.size())),
             BIO_free);
  if (!bio) { return ErrorResult<bool>("failed to allocate OpenSSL BIO"); }

  EvpPkeyPtr public_key(
      PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  if (!public_key) {
    return ErrorResult<bool>("failed to read public key PEM: %s",
                             OpenSslErrorString().c_str());
  }

  if (EVP_PKEY_base_id(public_key.get()) != EVP_PKEY_ED25519) {
    return ErrorResult<bool>("public key PEM does not contain an Ed25519 key");
  }

  EvpMdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx) {
    return ErrorResult<bool>("failed to allocate OpenSSL digest context");
  }

  if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr,
                           public_key.get())
      <= 0) {
    return ErrorResult<bool>("failed to initialize Ed25519 verification: %s",
                             OpenSslErrorString().c_str());
  }

  auto canonical = CanonicalizeSubscriptionContract(contract);
  int verify_result = EVP_DigestVerify(
      ctx.get(), contract.signature.data(), contract.signature.size(),
      reinterpret_cast<const unsigned char*>(canonical.data()),
      canonical.size());
  if (verify_result == 1) { return result<bool>{true}; }
  if (verify_result == 0) { return result<bool>{false}; }

  return ErrorResult<bool>("OpenSSL signature verification failed: %s",
                           OpenSslErrorString().c_str());
}

result<std::vector<std::uint8_t>> SignSubscriptionContract(
    const SubscriptionContract& contract,
    EVP_PKEY* private_key)
{
  if (!private_key) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "private key must not be null");
  }
  if (EVP_PKEY_base_id(private_key) != EVP_PKEY_ED25519) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "private key does not contain an Ed25519 key");
  }

  EvpMdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "failed to allocate OpenSSL digest context");
  }

  if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, private_key)
      <= 0) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "failed to initialize Ed25519 signing: %s",
        OpenSslErrorString().c_str());
  }

  auto canonical = CanonicalizeSubscriptionContract(contract);
  size_t signature_size = 0;
  if (EVP_DigestSign(ctx.get(), nullptr, &signature_size,
                     reinterpret_cast<const unsigned char*>(canonical.data()),
                     canonical.size())
      <= 0) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "OpenSSL signature generation failed: %s",
        OpenSslErrorString().c_str());
  }

  std::vector<std::uint8_t> signature(signature_size);
  if (EVP_DigestSign(ctx.get(), signature.data(), &signature_size,
                     reinterpret_cast<const unsigned char*>(canonical.data()),
                     canonical.size())
      <= 0) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "OpenSSL signature generation failed: %s",
        OpenSslErrorString().c_str());
  }

  signature.resize(signature_size);

  if (signature.size() != kEd25519SignatureSize) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "generated signature has unexpected size %d",
        static_cast<int>(signature.size()));
  }

  return result<std::vector<std::uint8_t>>{std::move(signature)};
}

result<std::vector<std::uint8_t>> SignSubscriptionContract(
    const SubscriptionContract& contract,
    std::string_view private_key_pem)
{
  if (private_key_pem.empty()) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "private key PEM must not be empty");
  }

  BioPtr bio(BIO_new_mem_buf(private_key_pem.data(),
                             static_cast<int>(private_key_pem.size())),
             BIO_free);
  if (!bio) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "failed to allocate OpenSSL BIO");
  }

  EvpPkeyPtr private_key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
      EVP_PKEY_free);
  if (!private_key) {
    return ErrorResult<std::vector<std::uint8_t>>(
        "failed to read private key PEM: %s", OpenSslErrorString().c_str());
  }

  return SignSubscriptionContract(contract, private_key.get());
}

ContractValidity EvaluateSubscriptionContractValidity(
    const CivilDate& expiration_date,
    std::time_t now,
    int warning_days)
{
  if (warning_days < 0) { warning_days = 0; }

  std::int64_t current_day = FloorDivide(static_cast<std::int64_t>(now),
                                         static_cast<std::int64_t>(86400));
  std::int64_t expiration_day = DaysFromCivil(expiration_date);

  if (current_day > expiration_day) { return ContractValidity::kExpired; }

  if ((expiration_day - current_day) <= warning_days) {
    return ContractValidity::kExpiringSoon;
  }

  return ContractValidity::kValid;
}
}  // namespace subscription
