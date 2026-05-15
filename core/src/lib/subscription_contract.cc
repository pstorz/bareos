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

result<ContractDateTime> ParseRfc3339UtcTimestamp(std::string_view input,
                                                  const char* field)
{
  if (input.size() != 20 || input[4] != '-' || input[7] != '-'
      || input[10] != 'T' || input[13] != ':' || input[16] != ':'
      || input[19] != 'Z') {
    return ErrorResult<ContractDateTime>(
        "%s must use the RFC 3339 UTC format YYYY-MM-DDTHH:MM:SSZ", field);
  }

  auto year_result = ParseUnsignedInteger(input.substr(0, 4), field);
  if (year_result.holds_error()) {
    return ErrorResult<ContractDateTime>("%s",
                                         year_result.error_unchecked().c_str());
  }
  auto month_result = ParseUnsignedInteger(input.substr(5, 2), field);
  if (month_result.holds_error()) {
    return ErrorResult<ContractDateTime>(
        "%s", month_result.error_unchecked().c_str());
  }
  auto day_result = ParseUnsignedInteger(input.substr(8, 2), field);
  if (day_result.holds_error()) {
    return ErrorResult<ContractDateTime>("%s",
                                         day_result.error_unchecked().c_str());
  }
  auto hour_result = ParseUnsignedInteger(input.substr(11, 2), field);
  if (hour_result.holds_error()) {
    return ErrorResult<ContractDateTime>("%s",
                                         hour_result.error_unchecked().c_str());
  }
  auto minute_result = ParseUnsignedInteger(input.substr(14, 2), field);
  if (minute_result.holds_error()) {
    return ErrorResult<ContractDateTime>(
        "%s", minute_result.error_unchecked().c_str());
  }
  auto second_result = ParseUnsignedInteger(input.substr(17, 2), field);
  if (second_result.holds_error()) {
    return ErrorResult<ContractDateTime>(
        "%s", second_result.error_unchecked().c_str());
  }

  ContractDateTime parsed{static_cast<int>(*year_result.value()),
                          static_cast<int>(*month_result.value()),
                          static_cast<int>(*day_result.value()),
                          static_cast<int>(*hour_result.value()),
                          static_cast<int>(*minute_result.value()),
                          static_cast<int>(*second_result.value()),
                          false};

  if (parsed.month < 1 || parsed.month > 12) {
    return ErrorResult<ContractDateTime>("%s month must be in the range 1-12",
                                         field);
  }
  if (parsed.day < 1 || parsed.day > DaysInMonth(parsed.year, parsed.month)) {
    return ErrorResult<ContractDateTime>("%s day is out of range", field);
  }
  if (parsed.hour < 0 || parsed.hour > 23) {
    return ErrorResult<ContractDateTime>("%s hour must be in the range 0-23",
                                         field);
  }
  if (parsed.minute < 0 || parsed.minute > 59) {
    return ErrorResult<ContractDateTime>("%s minute must be in the range 0-59",
                                         field);
  }
  if (parsed.second < 0 || parsed.second > 59) {
    return ErrorResult<ContractDateTime>("%s second must be in the range 0-59",
                                         field);
  }

  return result<ContractDateTime>{parsed};
}

std::int64_t DaysFromCivil(const ContractDateTime& date)
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

std::int64_t SecondsFromContractDateTime(const ContractDateTime& date_time)
{
  std::int64_t seconds = DaysFromCivil(date_time) * 86400
                         + static_cast<std::int64_t>(date_time.hour) * 3600
                         + static_cast<std::int64_t>(date_time.minute) * 60
                         + static_cast<std::int64_t>(date_time.second);
  if (date_time.date_only) { seconds += 86399; }
  return seconds;
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

result<std::optional<std::string>> GetOptionalJsonString(json_t* root,
                                                         const char* key)
{
  json_t* value = json_object_get(root, key);
  if (!value || json_is_null(value)) {
    return result<std::optional<std::string>>{std::optional<std::string>{}};
  }
  if (!json_is_string(value)) {
    return ErrorResult<std::optional<std::string>>(
        "subscription contract key '%s' must be a JSON string", key);
  }

  const char* string_value = json_string_value(value);
  if (!string_value) {
    return ErrorResult<std::optional<std::string>>(
        "subscription contract key '%s' must be a JSON string", key);
  }

  return result<std::optional<std::string>>{
      std::optional<std::string>{string_value}};
}

result<std::optional<bool>> GetOptionalJsonBoolean(json_t* root,
                                                   const char* key)
{
  json_t* value = json_object_get(root, key);
  if (!value || json_is_null(value)) {
    return result<std::optional<bool>>{std::optional<bool>{}};
  }
  if (!json_is_boolean(value)) {
    return ErrorResult<std::optional<bool>>(
        "subscription contract key '%s' must be a JSON boolean", key);
  }

  return result<std::optional<bool>>{
      std::optional<bool>{json_boolean_value(value) != 0}};
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

result<bool> AddSupportJson(json_t* root, const SubscriptionContract& contract)
{
  if (contract.support_level) {
    json_object_set_new(root, "support_level",
                        json_stringn(contract.support_level->data(),
                                     contract.support_level->size()));
  }
  if (contract.support_rear.has_value()) {
    json_object_set_new(root, "support_rear",
                        json_boolean(*contract.support_rear));
  }
  return result<bool>{true};
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
      = GetOptionalJsonString(root.get(), "customer_name");
  if (customer_name_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", customer_name_result.error_unchecked().c_str());
  }
  if (customer_name_result.value()->has_value()
      && !IsValidUtf8Text(customer_name_result.value()->value())) {
    return ErrorResult<SubscriptionContract>(
        "customer_name must be valid UTF-8 without control characters");
  }
  contract.customer_name = std::move(customer_name_result.value_unchecked());

  auto file_type_result = GetRequiredJsonString(root.get(), "file_type");
  if (file_type_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", file_type_result.error_unchecked().c_str());
  }
  if (*file_type_result.value() != kSubscriptionContractFileType) {
    return ErrorResult<SubscriptionContract>(
        "file_type must be '%s'",
        std::string(kSubscriptionContractFileType).c_str());
  }
  contract.file_type = std::string(*file_type_result.value());

  auto customer_contact_name_result
      = GetOptionalJsonString(root.get(), "customer_contact_name");
  if (customer_contact_name_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", customer_contact_name_result.error_unchecked().c_str());
  }
  if (customer_contact_name_result.value()->has_value()
      && !IsValidUtf8Text(customer_contact_name_result.value()->value())) {
    return ErrorResult<SubscriptionContract>(
        "customer_contact_name must be valid UTF-8 without control characters");
  }
  contract.customer_contact_name
      = std::move(customer_contact_name_result.value_unchecked());

  auto customer_contact_address_result
      = GetOptionalJsonString(root.get(), "customer_contact_address");
  if (customer_contact_address_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", customer_contact_address_result.error_unchecked().c_str());
  }
  if (customer_contact_address_result.value()->has_value()
      && !IsValidUtf8Text(customer_contact_address_result.value()->value())) {
    return ErrorResult<SubscriptionContract>(
        "customer_contact_address must be valid UTF-8 without control "
        "characters");
  }
  contract.customer_contact_address
      = std::move(customer_contact_address_result.value_unchecked());

  auto customer_contact_email_result
      = GetOptionalJsonString(root.get(), "customer_contact_email");
  if (customer_contact_email_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", customer_contact_email_result.error_unchecked().c_str());
  }
  if (customer_contact_email_result.value()->has_value()
      && !IsValidUtf8Text(customer_contact_email_result.value()->value())) {
    return ErrorResult<SubscriptionContract>(
        "customer_contact_email must be valid UTF-8 without control "
        "characters");
  }
  contract.customer_contact_email
      = std::move(customer_contact_email_result.value_unchecked());

  auto issued_by_result = GetOptionalJsonString(root.get(), "issued_by");
  if (issued_by_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", issued_by_result.error_unchecked().c_str());
  }
  if (issued_by_result.value()->has_value()
      && !IsValidUtf8Text(issued_by_result.value()->value())) {
    return ErrorResult<SubscriptionContract>(
        "issued_by must be valid UTF-8 without control characters");
  }
  contract.issued_by = std::move(issued_by_result.value_unchecked());

  auto issued_at_result = GetOptionalJsonString(root.get(), "issued_at");
  if (issued_at_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", issued_at_result.error_unchecked().c_str());
  }
  if (issued_at_result.value()->has_value()) {
    auto issued_at = ParseRfc3339UtcTimestamp(issued_at_result.value()->value(),
                                              "issued_at");
    if (issued_at.holds_error()) {
      return ErrorResult<SubscriptionContract>(
          "%s", issued_at.error_unchecked().c_str());
    }
    contract.issued_at = *issued_at.value();
  }

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

  auto support_level_result
      = GetOptionalJsonString(root.get(), "support_level");
  if (support_level_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", support_level_result.error_unchecked().c_str());
  }
  if (support_level_result.value()->has_value()
      && !IsValidUtf8Text(support_level_result.value()->value())) {
    return ErrorResult<SubscriptionContract>(
        "support_level must be valid UTF-8 without control characters");
  }
  contract.support_level = std::move(support_level_result.value_unchecked());

  auto support_rear_result = GetOptionalJsonBoolean(root.get(), "support_rear");
  if (support_rear_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", support_rear_result.error_unchecked().c_str());
  }
  contract.support_rear = *support_rear_result.value();

  json_t* legacy_support = json_object_get(root.get(), "support");
  if (legacy_support) {
    return ErrorResult<SubscriptionContract>(
        "subscription contract key 'support' is no longer supported; use "
        "'support_level' and 'support_rear'");
  }

  auto expiration_date_result
      = GetRequiredJsonString(root.get(), "expiration_date");
  if (expiration_date_result.holds_error()) {
    return ErrorResult<SubscriptionContract>(
        "%s", expiration_date_result.error_unchecked().c_str());
  }
  auto parsed_date = ParseRfc3339UtcTimestamp(*expiration_date_result.value(),
                                              "expiration_date");
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

std::string FormatSubscriptionContractDateTime(
    const ContractDateTime& date_time)
{
  char buffer[21];
  if (date_time.date_only) {
    Bsnprintf(buffer, 11, "%04d-%02d-%02d", date_time.year, date_time.month,
              date_time.day);
  } else {
    Bsnprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
              date_time.year, date_time.month, date_time.day, date_time.hour,
              date_time.minute, date_time.second);
  }
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
  if (contract.file_type) {
    json_object_set_new(
        document.get(), "file_type",
        json_stringn(contract.file_type->data(), contract.file_type->size()));
  }
  if (contract.customer_name) {
    json_object_set_new(document.get(), "customer_name",
                        json_stringn(contract.customer_name->data(),
                                     contract.customer_name->size()));
  }
  if (contract.customer_contact_name) {
    json_object_set_new(document.get(), "customer_contact_name",
                        json_stringn(contract.customer_contact_name->data(),
                                     contract.customer_contact_name->size()));
  }
  if (contract.customer_contact_address) {
    json_object_set_new(
        document.get(), "customer_contact_address",
        json_stringn(contract.customer_contact_address->data(),
                     contract.customer_contact_address->size()));
  }
  if (contract.customer_contact_email) {
    json_object_set_new(document.get(), "customer_contact_email",
                        json_stringn(contract.customer_contact_email->data(),
                                     contract.customer_contact_email->size()));
  }
  json_object_set_new(document.get(), "backup_units",
                      json_integer(contract.backup_units));
  if (contract.issued_by) {
    json_object_set_new(
        document.get(), "issued_by",
        json_stringn(contract.issued_by->data(), contract.issued_by->size()));
  }
  if (contract.issued_at) {
    auto issued_at = FormatSubscriptionContractDateTime(*contract.issued_at);
    json_object_set_new(document.get(), "issued_at",
                        json_stringn(issued_at.data(), issued_at.size()));
  }
  auto support_result = AddSupportJson(document.get(), contract);
  if (support_result.holds_error()) {
    return ErrorResult<std::string>("%s",
                                    support_result.error_unchecked().c_str());
  }
  auto expiration_date
      = FormatSubscriptionContractDateTime(contract.expiration_date);
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
  if (contract.file_type) {
    json_object_set_new(
        canonical.get(), "file_type",
        json_stringn(contract.file_type->data(), contract.file_type->size()));
  }
  if (contract.customer_name) {
    json_object_set_new(canonical.get(), "customer_name",
                        json_stringn(contract.customer_name->data(),
                                     contract.customer_name->size()));
  }
  if (contract.customer_contact_name) {
    json_object_set_new(canonical.get(), "customer_contact_name",
                        json_stringn(contract.customer_contact_name->data(),
                                     contract.customer_contact_name->size()));
  }
  if (contract.customer_contact_address) {
    json_object_set_new(
        canonical.get(), "customer_contact_address",
        json_stringn(contract.customer_contact_address->data(),
                     contract.customer_contact_address->size()));
  }
  if (contract.customer_contact_email) {
    json_object_set_new(canonical.get(), "customer_contact_email",
                        json_stringn(contract.customer_contact_email->data(),
                                     contract.customer_contact_email->size()));
  }
  json_object_set_new(canonical.get(), "backup_units",
                      json_integer(contract.backup_units));
  if (contract.issued_by) {
    json_object_set_new(
        canonical.get(), "issued_by",
        json_stringn(contract.issued_by->data(), contract.issued_by->size()));
  }
  if (contract.issued_at) {
    auto issued_at = FormatSubscriptionContractDateTime(*contract.issued_at);
    json_object_set_new(canonical.get(), "issued_at",
                        json_stringn(issued_at.data(), issued_at.size()));
  }
  auto support_result = AddSupportJson(canonical.get(), contract);
  if (support_result.holds_error()) { return {}; }
  auto expiration_date
      = FormatSubscriptionContractDateTime(contract.expiration_date);
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
    const ContractDateTime& expiration_date,
    std::time_t now,
    int warning_days)
{
  if (warning_days < 0) { warning_days = 0; }

  std::int64_t current_second = static_cast<std::int64_t>(now);
  std::int64_t expiration_second = SecondsFromContractDateTime(expiration_date);

  if (current_second > expiration_second) { return ContractValidity::kExpired; }

  if ((expiration_second - current_second)
      <= static_cast<std::int64_t>(warning_days) * 86400) {
    return ContractValidity::kExpiringSoon;
  }

  return ContractValidity::kValid;
}
}  // namespace subscription
