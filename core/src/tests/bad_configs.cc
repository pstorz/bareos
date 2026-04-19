/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2022-2024 Bareos GmbH & Co. KG

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

#include "testing_dir_common.h"

#include "dird/ua.h"
#include "include/jcr.h"
#include "dird/ua_configure.cc"

TEST(BadConfig, changing_pw_type)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/changing_pw_type.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser; /* set the director global variable */

  EXPECT_FALSE(parser->ParseConfig());

  delete parser;
}

TEST(BadConfig, subscription_invalid_signature)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/subscription-invalid-signature.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser;

  EXPECT_FALSE(parser->ParseConfig());

  delete parser;
}

TEST(BadConfig, subscription_manual_override)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/subscription-manual-override.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser;

  EXPECT_FALSE(parser->ParseConfig());

  delete parser;
}

TEST(BadConfig, subscription_unsupported_algorithm)
{
  InitDirGlobals();
  std::string path_to_config
      = std::string("configs/bad_configs/subscription-unsupported-algorithm.conf");

  auto* parser = directordaemon::InitDirConfig(path_to_config.c_str(), M_INFO);

  ASSERT_NE(parser, nullptr);
  directordaemon::my_config = parser;

  EXPECT_FALSE(parser->ParseConfig());

  delete parser;
}
