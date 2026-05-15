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

#include "dird/subscription_manager.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "include/bareos.h"
#include "dird/dird_conf.h"
#include "dird/dird_globals.h"

namespace directordaemon {
namespace {
struct FileReadResult {
  bool missing{};
  std::string content;
  std::string error;
};

FileReadResult ReadSubscriptionFile(std::string_view file_path)
{
  struct stat statp{};
  std::string file_path_string(file_path);
  if (stat(file_path_string.c_str(), &statp) != 0) {
    int saved_errno = errno;
    if (saved_errno == ENOENT || saved_errno == ENOTDIR) {
      return FileReadResult{true, {}, {}};
    }

    return FileReadResult{
        false, {}, std::string("stat failed: ") + std::strerror(saved_errno)};
  }

  if (!S_ISREG(statp.st_mode)) {
    return FileReadResult{
        false, {}, "subscription contract path is not a regular file"};
  }

  if (statp.st_size > 0
      && static_cast<std::uint64_t>(statp.st_size)
             > subscription::kMaxSubscriptionContractFileSize) {
    std::ostringstream stream;
    stream << "subscription contract exceeds the maximum supported size of "
           << subscription::kMaxSubscriptionContractFileSize << " bytes";
    return FileReadResult{false, {}, stream.str()};
  }

  std::ifstream file(file_path_string, std::ios::binary);
  if (!file.is_open()) {
    int saved_errno = errno;
    return FileReadResult{
        false,
        {},
        std::string("failed to open file: ") + std::strerror(saved_errno)};
  }

  std::string content;
  if (statp.st_size > 0) {
    content.resize(static_cast<std::size_t>(statp.st_size));
    file.read(content.data(), content.size());
    if (!file) {
      return FileReadResult{false, {}, "failed to read subscription contract"};
    }
  }

  return FileReadResult{false, std::move(content), {}};
}

const SubscriptionTrustedPublicKey* FindTrustedKey(
    std::string_view key_id,
    const SubscriptionTrustedPublicKey* trusted_keys,
    std::size_t trusted_key_count)
{
  for (std::size_t i = 0; i < trusted_key_count; ++i) {
    if (trusted_keys[i].key_id && key_id == trusted_keys[i].key_id) {
      return &trusted_keys[i];
    }
  }

  return nullptr;
}

bool ShouldWarn(const SubscriptionContractSnapshot& snapshot)
{
  switch (snapshot.load_state) {
    case SubscriptionContractLoadState::kNotConfigured:
      return false;
    case SubscriptionContractLoadState::kValid:
      return snapshot.validity == subscription::ContractValidity::kExpiringSoon
             || snapshot.validity == subscription::ContractValidity::kExpired;
    default:
      return true;
  }
}

std::string BuildWarningMessage(const SubscriptionContractSnapshot& snapshot)
{
  switch (snapshot.load_state) {
    case SubscriptionContractLoadState::kNotConfigured:
      return {};
    case SubscriptionContractLoadState::kFileMissing:
      return "Subscription contract file '" + snapshot.file_path
             + "' was not found. Falling back to the legacy Director "
               "Subscriptions setting.";
    case SubscriptionContractLoadState::kReadError:
      return "Failed to read subscription contract file '" + snapshot.file_path
             + "': " + snapshot.detail
             + ". Falling back to the legacy Director Subscriptions setting.";
    case SubscriptionContractLoadState::kParseError:
      return "Failed to parse subscription contract file '" + snapshot.file_path
             + "': " + snapshot.detail
             + ". Falling back to the legacy Director Subscriptions setting.";
    case SubscriptionContractLoadState::kSignatureInvalid:
      return "Failed to verify subscription contract file '"
             + snapshot.file_path + "': " + snapshot.detail
             + ". Falling back to the legacy Director Subscriptions setting.";
    case SubscriptionContractLoadState::kKeyUnknown:
      return "Subscription contract file '" + snapshot.file_path
             + "' references an unknown Bareos signing key: " + snapshot.detail
             + ". Falling back to the legacy Director Subscriptions setting.";
    case SubscriptionContractLoadState::kValid: {
      if (!snapshot.contract || !snapshot.validity) { return {}; }

      auto expiration = subscription::FormatSubscriptionContractExpirationDate(
          snapshot.contract->expiration_date);
      if (*snapshot.validity == subscription::ContractValidity::kExpired) {
        return "Subscription contract file '" + snapshot.file_path
               + "' for customer '" + snapshot.contract->customer_name
               + "' expired on " + expiration + ".";
      }

      if (*snapshot.validity == subscription::ContractValidity::kExpiringSoon) {
        return "Subscription contract file '" + snapshot.file_path
               + "' for customer '" + snapshot.contract->customer_name
               + "' expires on " + expiration + ".";
      }

      return {};
    }
  }

  return {};
}

std::array<SubscriptionTrustedPublicKey, 0> kBuiltInTrustedSubscriptionKeys{};

SubscriptionContractManager g_subscription_contract_manager(
    kBuiltInTrustedSubscriptionKeys.data(),
    kBuiltInTrustedSubscriptionKeys.size(),
    kDefaultSubscriptionContractFile,
    [](const SubscriptionContractSnapshot& snapshot) {
      auto warning = BuildWarningMessage(snapshot);
      if (!warning.empty()) {
        Jmsg(nullptr, M_WARNING, 0, "%s\n", warning.c_str());
      }
    });
}  // namespace

SubscriptionContractManager::SubscriptionContractManager(
    const SubscriptionTrustedPublicKey* trusted_keys,
    std::size_t trusted_key_count,
    std::string default_file_path,
    LogCallback log_callback)
    : trusted_keys_(trusted_keys)
    , trusted_key_count_(trusted_key_count)
    , default_file_path_(std::move(default_file_path))
    , log_callback_(std::move(log_callback))
{
  cached_.snapshot.file_path = default_file_path_;
}

SubscriptionContractSnapshot SubscriptionContractManager::Reload(
    std::string_view configured_file,
    std::time_t now)
{
  std::lock_guard guard(mutex_);
  return Update(configured_file, now, true);
}

SubscriptionContractSnapshot SubscriptionContractManager::RefreshIfChanged(
    std::string_view configured_file,
    std::time_t now)
{
  std::lock_guard guard(mutex_);
  return Update(configured_file, now, false);
}

SubscriptionContractSnapshot SubscriptionContractManager::GetSnapshot(
    std::time_t now) const
{
  std::lock_guard guard(mutex_);
  auto snapshot = cached_.snapshot;
  UpdateValidity(snapshot, now);
  return snapshot;
}

SubscriptionContractSnapshot SubscriptionContractManager::Update(
    std::string_view configured_file,
    std::time_t now,
    bool force_reload)
{
  auto previous = cached_.snapshot;
  bool explicitly_configured = !configured_file.empty();
  auto file_path = ResolveFilePath(configured_file);
  auto read_result = ReadSubscriptionFile(file_path);

  SubscriptionContractSnapshot next;
  if (read_result.missing) {
    next = MakeMissingSnapshot(file_path, explicitly_configured);
    cached_.file_content.clear();
  } else if (!read_result.error.empty()) {
    next = MakeErrorSnapshot(SubscriptionContractLoadState::kReadError,
                             file_path, explicitly_configured,
                             std::move(read_result.error));
    cached_.file_content.clear();
  } else {
    bool same_target
        = cached_.configured_file == configured_file
          && cached_.snapshot.file_path == file_path
          && cached_.snapshot.explicitly_configured == explicitly_configured;
    if (!force_reload && same_target
        && cached_.file_content == read_result.content) {
      next = cached_.snapshot;
      UpdateValidity(next, now);
    } else {
      next = EvaluateFileContent(file_path, explicitly_configured,
                                 std::move(read_result.content), now);
    }
  }

  cached_.snapshot = next;
  cached_.configured_file = std::string(configured_file);
  EmitTransitionWarning(previous, cached_.snapshot);
  return cached_.snapshot;
}

SubscriptionContractSnapshot SubscriptionContractManager::EvaluateFileContent(
    std::string_view file_path,
    bool explicitly_configured,
    std::string&& file_content,
    std::time_t now)
{
  cached_.file_content = std::move(file_content);

  auto parsed = subscription::ParseSubscriptionContract(cached_.file_content);
  if (parsed.holds_error()) {
    return MakeErrorSnapshot(SubscriptionContractLoadState::kParseError,
                             file_path, explicitly_configured,
                             parsed.error_unchecked().c_str());
  }

  auto* trusted_key = FindTrustedKey(parsed.value_unchecked().key_id,
                                     trusted_keys_, trusted_key_count_);
  if (!trusted_key) {
    return MakeErrorSnapshot(SubscriptionContractLoadState::kKeyUnknown,
                             file_path, explicitly_configured,
                             parsed.value_unchecked().key_id);
  }

  auto verified = subscription::VerifySubscriptionContractSignature(
      parsed.value_unchecked(), trusted_key->public_key_pem);
  if (verified.holds_error()) {
    return MakeErrorSnapshot(SubscriptionContractLoadState::kSignatureInvalid,
                             file_path, explicitly_configured,
                             verified.error_unchecked().c_str());
  }
  if (!verified.value_unchecked()) {
    return MakeErrorSnapshot(SubscriptionContractLoadState::kSignatureInvalid,
                             file_path, explicitly_configured,
                             "signature mismatch");
  }

  SubscriptionContractSnapshot snapshot;
  snapshot.load_state = SubscriptionContractLoadState::kValid;
  snapshot.explicitly_configured = explicitly_configured;
  snapshot.file_path = std::string(file_path);
  snapshot.contract = parsed.value_unchecked();
  UpdateValidity(snapshot, now);
  return snapshot;
}

SubscriptionContractSnapshot SubscriptionContractManager::MakeMissingSnapshot(
    std::string_view file_path,
    bool explicitly_configured)
{
  SubscriptionContractSnapshot snapshot;
  snapshot.load_state = explicitly_configured
                            ? SubscriptionContractLoadState::kFileMissing
                            : SubscriptionContractLoadState::kNotConfigured;
  snapshot.explicitly_configured = explicitly_configured;
  snapshot.file_path = std::string(file_path);
  if (explicitly_configured) {
    snapshot.detail = "subscription contract file not found";
  }
  return snapshot;
}

SubscriptionContractSnapshot SubscriptionContractManager::MakeErrorSnapshot(
    SubscriptionContractLoadState state,
    std::string_view file_path,
    bool explicitly_configured,
    std::string detail)
{
  SubscriptionContractSnapshot snapshot;
  snapshot.load_state = state;
  snapshot.explicitly_configured = explicitly_configured;
  snapshot.file_path = std::string(file_path);
  snapshot.detail = std::move(detail);
  return snapshot;
}

void SubscriptionContractManager::UpdateValidity(
    SubscriptionContractSnapshot& snapshot,
    std::time_t now) const
{
  if (snapshot.load_state == SubscriptionContractLoadState::kValid
      && snapshot.contract) {
    snapshot.validity = subscription::EvaluateSubscriptionContractValidity(
        snapshot.contract->expiration_date, now);
  } else {
    snapshot.validity.reset();
  }
}

void SubscriptionContractManager::EmitTransitionWarning(
    const SubscriptionContractSnapshot& previous,
    const SubscriptionContractSnapshot& current) const
{
  if (!log_callback_ || !ShouldWarn(current)) { return; }
  if (BuildTransitionKey(previous) == BuildTransitionKey(current)) { return; }
  log_callback_(current);
}

std::string SubscriptionContractManager::BuildTransitionKey(
    const SubscriptionContractSnapshot& snapshot) const
{
  std::ostringstream key;
  key << static_cast<int>(snapshot.load_state) << '\n'
      << snapshot.explicitly_configured << '\n'
      << snapshot.file_path << '\n'
      << snapshot.detail;

  if (snapshot.validity.has_value()) {
    key << '\n' << static_cast<int>(*snapshot.validity);
  }

  if (snapshot.contract.has_value()) {
    key << '\n'
        << snapshot.contract->customer_name << '\n'
        << snapshot.contract->backup_units << '\n'
        << subscription::FormatSubscriptionContractExpirationDate(
               snapshot.contract->expiration_date);
  }

  return key.str();
}

std::string SubscriptionContractManager::ResolveFilePath(
    std::string_view configured_file) const
{
  if (!configured_file.empty()) { return std::string(configured_file); }
  return default_file_path_;
}

void LoadSubscriptionContract()
{
  std::string configured_file;
  if (me && me->subscription_file) { configured_file = me->subscription_file; }
  g_subscription_contract_manager.Reload(configured_file);
}

void RefreshSubscriptionContractIfChanged()
{
  std::string configured_file;
  if (me && me->subscription_file) { configured_file = me->subscription_file; }
  g_subscription_contract_manager.RefreshIfChanged(configured_file);
}

SubscriptionContractSnapshot GetSubscriptionContractSnapshot()
{
  return g_subscription_contract_manager.GetSnapshot();
}

}  // namespace directordaemon
