/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "gtest/gtest.h"
#else
#  include "gtest/gtest.h"
#  include "include/bareos.h"
#endif

#include "cats/cats.h"

class MockCatalogDb final : public BareosDb {
 public:
  MockCatalogDb() { is_private_ = true; }

  void StoreSqlError(std::string sqlstate,
                     std::string message,
                     std::string phase,
                     SqlErrorClassification classification)
  {
    SetLastSqlError(std::move(sqlstate), std::move(message), std::move(phase),
                    classification);
  }

  LastSqlError ReadSqlError() { return GetLastSqlError(); }

  void ResetSqlError() { ClearLastSqlError(); }

  const char* OpenDatabase() override { return "mock"; }
  void CloseDatabase(JobControlRecord*) override {}
  void StartTransaction(JobControlRecord*) override {}
  void EndTransaction(JobControlRecord*) override {}

 private:
  void SqlFieldSeek(int) override {}
  int SqlNumFields(void) override { return 0; }
  void SqlFreeResult(void) override {}
  SQL_ROW SqlFetchRow(void) override { return nullptr; }
  bool SqlQueryWithHandler(const char*, DB_RESULT_HANDLER*, void*) override
  {
    return true;
  }
  bool SqlQueryWithoutHandler(const char*, query_flags) override { return true; }
  const char* sql_strerror(void) override { return ""; }
  void SqlDataSeek(int) override {}
  int SqlAffectedRows(void) override { return 0; }
  uint64_t SqlInsertAutokeyRecord(const char*, const char*) override
  {
    return 0;
  }
  SQL_FIELD* SqlFetchField(void) override { return nullptr; }
  bool SqlFieldIsNotNull(int) override { return true; }
  bool SqlFieldIsNumeric(int) override { return true; }
  bool SqlBatchStartFileTable(JobControlRecord*) override { return true; }
  bool SqlBatchEndFileTable(JobControlRecord*, const char*) override
  {
    return true;
  }
  bool SqlBatchInsertFileTable(JobControlRecord*, AttributesDbRecord*) override
  {
    return true;
  }
};

TEST(CatalogSqlError, metadata_roundtrip)
{
  MockCatalogDb db;
  db.StoreSqlError("XX001", "invalid page in block", "cursor fetch",
                   BareosDb::SqlErrorClassification::kLikelyCatalogCorruption);

  auto error = db.ReadSqlError();
  EXPECT_TRUE(error.has_error);
  EXPECT_EQ(error.sqlstate, "XX001");
  EXPECT_EQ(error.message, "invalid page in block");
  EXPECT_EQ(error.phase, "cursor fetch");
  EXPECT_EQ(error.classification,
            BareosDb::SqlErrorClassification::kLikelyCatalogCorruption);
}

TEST(CatalogSqlError, metadata_clear)
{
  MockCatalogDb db;
  db.StoreSqlError("", "generic sql error", "query",
                   BareosDb::SqlErrorClassification::kGeneric);
  db.ResetSqlError();

  auto error = db.ReadSqlError();
  EXPECT_FALSE(error.has_error);
  EXPECT_EQ(error.classification, BareosDb::SqlErrorClassification::kNone);
  EXPECT_TRUE(error.sqlstate.empty());
  EXPECT_TRUE(error.message.empty());
  EXPECT_TRUE(error.phase.empty());
}
