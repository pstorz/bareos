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

#include "include/bareos.h"
#include "lib/cli.h"
#include "lib/subscription_contract.h"

#include <fstream>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/store.h>
#if OPENSSL_VERSION_MAJOR >= 3
#  include <openssl/provider.h>
#endif
#include <memory>
#include <sstream>
#include <vector>

namespace {
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using OsslStoreCtxPtr
    = std::unique_ptr<OSSL_STORE_CTX, decltype(&OSSL_STORE_close)>;
using OsslStoreInfoPtr
    = std::unique_ptr<OSSL_STORE_INFO, decltype(&OSSL_STORE_INFO_free)>;
#if OPENSSL_VERSION_MAJOR >= 3
using OsslProviderPtr
    = std::unique_ptr<OSSL_PROVIDER, decltype(&OSSL_PROVIDER_unload)>;
#endif

std::string ReadFileOrStdin(const std::string& path)
{
  if (path == "-") {
    std::ostringstream stream;
    stream << std::cin.rdbuf();
    return stream.str();
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open input file: " + path);
  }

  std::ostringstream stream;
  stream << file.rdbuf();
  if (!file.good() && !file.eof()) {
    throw std::runtime_error("Failed to read input file: " + path);
  }

  return stream.str();
}

void WriteFileOrStdout(const std::string& path, const std::string& content)
{
  if (path == "-") {
    std::cout << content;
    if (content.empty() || content.back() != '\n') { std::cout << '\n'; }
    return;
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open output file: " + path);
  }

  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (content.empty() || content.back() != '\n') { file.put('\n'); }
  if (!file.good()) {
    throw std::runtime_error("Failed to write output file: " + path);
  }
}

std::string OpenSslErrorString()
{
  unsigned long error = ERR_peek_last_error();
  if (error == 0) { return "unknown OpenSSL error"; }

  char buffer[256];
  ERR_error_string_n(error, buffer, sizeof(buffer));
  return buffer;
}

EvpPkeyPtr LoadPrivateKeyFromPem(const std::string& private_key_pem)
{
  BioPtr bio(BIO_new_mem_buf(private_key_pem.data(),
                             static_cast<int>(private_key_pem.size())),
             BIO_free);
  if (!bio) { throw std::runtime_error("Failed to allocate OpenSSL BIO"); }

  EVP_PKEY* raw_key
      = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (!raw_key) {
    throw std::runtime_error("Failed to read private key PEM: "
                             + OpenSslErrorString());
  }

  return EvpPkeyPtr(raw_key, EVP_PKEY_free);
}

EvpPkeyPtr LoadPrivateKeyFromUri(const std::string& private_key_uri)
{
  ERR_clear_error();
  OsslStoreCtxPtr store(OSSL_STORE_open(private_key_uri.c_str(), nullptr,
                                        nullptr, nullptr, nullptr),
                        OSSL_STORE_close);
  if (!store) {
    throw std::runtime_error("Failed to open private key URI: "
                             + OpenSslErrorString());
  }
  if (OSSL_STORE_expect(store.get(), OSSL_STORE_INFO_PKEY) != 1) {
    throw std::runtime_error("Failed to configure private key URI loader: "
                             + OpenSslErrorString());
  }

  EVP_PKEY* loaded_key = nullptr;
  for (;;) {
    ERR_clear_error();
    OsslStoreInfoPtr info(OSSL_STORE_load(store.get()), OSSL_STORE_INFO_free);
    if (!info) {
      if (OSSL_STORE_eof(store.get())) { break; }
      if (OSSL_STORE_error(store.get())) {
        if (loaded_key) { break; }
        throw std::runtime_error("Failed to load from private key URI: "
                                 + OpenSslErrorString());
      }
      continue;
    }

    if (OSSL_STORE_INFO_get_type(info.get()) != OSSL_STORE_INFO_PKEY) {
      continue;
    }

    EVP_PKEY* candidate = OSSL_STORE_INFO_get1_PKEY(info.get());
    if (!candidate) {
      throw std::runtime_error("Failed to extract private key from URI: "
                               + OpenSslErrorString());
    }

    if (loaded_key) {
      EVP_PKEY_free(candidate);
      EVP_PKEY_free(loaded_key);
      throw std::runtime_error(
          "Private key URI resolved to multiple private keys");
    }

    loaded_key = candidate;
  }

  if (!loaded_key) { throw std::runtime_error("No private key found in URI"); }

  return EvpPkeyPtr(loaded_key, EVP_PKEY_free);
}
}  // namespace

int main(int argc, char* argv[])
{
  setlocale(LC_ALL, "");
  tzset();
  bindtextdomain("bareos", LOCALEDIR);
  textdomain("bareos");

  CLI::App app;
  InitCLIApp(app, "The Bareos subscription contract signing tool.");

  std::string input_path;
  app.add_option("-i,--input", input_path,
                 "Read unsigned contract JSON from <file> or '-' for stdin.")
      ->required()
      ->type_name("<file>");

  std::string private_key_path;
  auto private_key_option
      = app.add_option("-k,--private-key", private_key_path,
                       "Read Ed25519 private key PEM from <file>.")
            ->check(CLI::ExistingFile)
            ->type_name("<file>");

  std::string private_key_uri;
  auto private_key_uri_option
      = app.add_option("--private-key-uri", private_key_uri,
                       "Load a private key from an OpenSSL store URI, for "
                       "example a pkcs11: URI.")
            ->type_name("<uri>");
  private_key_option->excludes(private_key_uri_option);
  private_key_uri_option->excludes(private_key_option);

  std::string output_path = "-";
  app.add_option("-o,--output", output_path,
                 "Write signed contract JSON to <file> or '-' for stdout.")
      ->type_name("<file>");

  bool compact = false;
  app.add_flag("--compact", compact,
               "Write compact JSON instead of human-readable pretty JSON.");

#if OPENSSL_VERSION_MAJOR >= 3
  std::vector<std::string> providers;
  app.add_option("--provider", providers,
                 "Load an OpenSSL provider before resolving the private key. "
                 "Repeat this option to load multiple providers.")
      ->type_name("<name>");

  std::string provider_path;
  app.add_option("--provider-path", provider_path,
                 "Set the OpenSSL provider search path before loading "
                 "providers.")
      ->type_name("<dir>");
#endif

  ParseBareosApp(app, argc, argv);

  try {
#if OPENSSL_VERSION_MAJOR >= 3
    std::vector<OsslProviderPtr> loaded_providers;
    if (!provider_path.empty()) {
      if (OSSL_PROVIDER_set_default_search_path(nullptr, provider_path.c_str())
          != 1) {
        throw std::runtime_error("Failed to set OpenSSL provider search path: "
                                 + OpenSslErrorString());
      }
    }
    for (const auto& provider_name : providers) {
      OSSL_PROVIDER* raw_provider
          = OSSL_PROVIDER_load(nullptr, provider_name.c_str());
      if (!raw_provider) {
        throw std::runtime_error("Failed to load OpenSSL provider '"
                                 + provider_name
                                 + "': " + OpenSslErrorString());
      }
      loaded_providers.emplace_back(raw_provider, OSSL_PROVIDER_unload);
    }
#endif

    auto input_json = ReadFileOrStdin(input_path);
    auto contract
        = subscription::ParseSubscriptionContractForSigning(input_json);
    if (contract.holds_error()) {
      std::cerr << contract.error_unchecked().c_str() << '\n';
      return 1;
    }

    EvpPkeyPtr private_key(nullptr, EVP_PKEY_free);
    if (!private_key_path.empty()) {
      private_key = LoadPrivateKeyFromPem(ReadFileOrStdin(private_key_path));
    } else if (!private_key_uri.empty()) {
      private_key = LoadPrivateKeyFromUri(private_key_uri);
    } else {
      std::cerr << "Either --private-key or --private-key-uri is required\n";
      return 1;
    }

    auto signature = subscription::SignSubscriptionContract(
        contract.value_unchecked(), private_key.get());
    if (signature.holds_error()) {
      std::cerr << signature.error_unchecked().c_str() << '\n';
      return 1;
    }

    auto signed_contract = contract.value_unchecked();
    signed_contract.signature = std::move(signature.value_unchecked());

    auto rendered
        = subscription::SerializeSubscriptionContract(signed_contract, compact);
    if (rendered.holds_error()) {
      std::cerr << rendered.error_unchecked().c_str() << '\n';
      return 1;
    }

    WriteFileOrStdout(output_path, rendered.value_unchecked());
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }

  return 0;
}
