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

#ifndef BAREOS_LIB_SUBSCRIPTION_CONTRACT_H_
#define BAREOS_LIB_SUBSCRIPTION_CONTRACT_H_

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <openssl/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/util.h"

namespace subscription {
constexpr uint64_t kCurrentSubscriptionContractFormatVersion = 1;
constexpr std::size_t kMaxSubscriptionContractFileSize = 16 * 1024;
constexpr std::size_t kEd25519SignatureSize = 64;
constexpr int kExpirationWarningDays = 60;

struct CivilDate {
  int year{};
  int month{};
  int day{};
};

inline bool operator==(const CivilDate& lhs, const CivilDate& rhs)
{
  return lhs.year == rhs.year && lhs.month == rhs.month && lhs.day == rhs.day;
}

enum class ContractValidity
{
  kValid,
  kExpiringSoon,
  kExpired
};

struct SubscriptionContract {
  struct Support {
    std::string level;
    bool rear_support{};
  };

  uint64_t format_version{kCurrentSubscriptionContractFormatVersion};
  std::string customer_name;
  uint64_t backup_units{};
  std::optional<Support> support;
  CivilDate expiration_date{};
  std::string key_id;
  std::vector<std::uint8_t> signature;
};

result<SubscriptionContract> ParseSubscriptionContract(std::string_view input);
result<SubscriptionContract> ParseSubscriptionContractForSigning(
    std::string_view input);
std::string CanonicalizeSubscriptionContract(
    const SubscriptionContract& contract);
std::string FormatSubscriptionContractExpirationDate(const CivilDate& date);
result<std::string> SerializeSubscriptionContract(
    const SubscriptionContract& contract,
    bool compact = false);
result<std::vector<std::uint8_t>> SignSubscriptionContract(
    const SubscriptionContract& contract,
    EVP_PKEY* private_key);
result<std::vector<std::uint8_t>> SignSubscriptionContract(
    const SubscriptionContract& contract,
    std::string_view private_key_pem);
result<bool> VerifySubscriptionContractSignature(
    const SubscriptionContract& contract,
    std::string_view public_key_pem);
ContractValidity EvaluateSubscriptionContractValidity(
    const CivilDate& expiration_date,
    std::time_t now,
    int warning_days = kExpirationWarningDays);
}  // namespace subscription

#endif  // BAREOS_LIB_SUBSCRIPTION_CONTRACT_H_
