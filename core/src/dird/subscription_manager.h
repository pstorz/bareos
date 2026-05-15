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

#ifndef BAREOS_DIRD_SUBSCRIPTION_MANAGER_H_
#define BAREOS_DIRD_SUBSCRIPTION_MANAGER_H_

#include <cstddef>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "lib/subscription_contract.h"

namespace directordaemon {

constexpr const char* kDefaultSubscriptionContractFile
    = CONFDIR "/subscription.json";

enum class SubscriptionContractLoadState
{
  kNotConfigured,
  kFileMissing,
  kReadError,
  kParseError,
  kSignatureInvalid,
  kKeyUnknown,
  kValid
};

struct SubscriptionTrustedPublicKey {
  const char* key_id;
  const char* public_key_pem;
};

struct SubscriptionContractSnapshot {
  SubscriptionContractLoadState load_state{
      SubscriptionContractLoadState::kNotConfigured};
  bool explicitly_configured{};
  std::string file_path;
  std::string detail;
  std::optional<subscription::SubscriptionContract> contract;
  std::optional<subscription::ContractValidity> validity;
};

class SubscriptionContractManager {
 public:
  using LogCallback = std::function<void(const SubscriptionContractSnapshot&)>;

  SubscriptionContractManager(const SubscriptionTrustedPublicKey* trusted_keys,
                              std::size_t trusted_key_count,
                              std::string default_file_path
                              = kDefaultSubscriptionContractFile,
                              LogCallback log_callback = {});

  SubscriptionContractSnapshot Reload(std::string_view configured_file,
                                      std::time_t now = std::time(nullptr));
  SubscriptionContractSnapshot RefreshIfChanged(
      std::string_view configured_file,
      std::time_t now = std::time(nullptr));
  SubscriptionContractSnapshot GetSnapshot(std::time_t now
                                           = std::time(nullptr)) const;

 private:
  struct CachedSnapshot {
    SubscriptionContractSnapshot snapshot;
    std::string configured_file;
    std::string file_content;
  };

  SubscriptionContractSnapshot Update(std::string_view configured_file,
                                      std::time_t now,
                                      bool force_reload);
  SubscriptionContractSnapshot EvaluateFileContent(std::string_view file_path,
                                                   bool explicitly_configured,
                                                   std::string&& file_content,
                                                   std::time_t now);
  SubscriptionContractSnapshot MakeMissingSnapshot(std::string_view file_path,
                                                   bool explicitly_configured);
  SubscriptionContractSnapshot MakeErrorSnapshot(
      SubscriptionContractLoadState state,
      std::string_view file_path,
      bool explicitly_configured,
      std::string detail);
  void UpdateValidity(SubscriptionContractSnapshot& snapshot,
                      std::time_t now) const;
  void EmitTransitionWarning(const SubscriptionContractSnapshot& previous,
                             const SubscriptionContractSnapshot& current) const;
  std::string BuildTransitionKey(
      const SubscriptionContractSnapshot& snapshot) const;
  std::string ResolveFilePath(std::string_view configured_file) const;

  const SubscriptionTrustedPublicKey* trusted_keys_;
  std::size_t trusted_key_count_;
  std::string default_file_path_;
  LogCallback log_callback_;
  mutable std::mutex mutex_;
  CachedSnapshot cached_;
};

void LoadSubscriptionContract();
void RefreshSubscriptionContractIfChanged();
SubscriptionContractSnapshot GetSubscriptionContractSnapshot();

}  // namespace directordaemon

#endif  // BAREOS_DIRD_SUBSCRIPTION_MANAGER_H_
