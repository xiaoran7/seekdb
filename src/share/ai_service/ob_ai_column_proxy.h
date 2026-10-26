/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_PROXY_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_PROXY_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_iarray.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ai_service/ob_ai_column_struct.h"

namespace oceanbase
{
namespace common
{
class ObISQLClient;
class ObMySQLTransaction;
}
namespace share
{

// Persistence for the AI column metadata and its task queue. The DDL path runs
// inside the DDL transaction, so both writers take a transaction; the reader
// only needs a SQL client.
class ObAiColumnProxy
{
public:
  ObAiColumnProxy() = default;
  ~ObAiColumnProxy() = default;

  // Inserts one __all_ai_column row. Fails with OB_ENTRY_EXIST when the AI
  // column is already recorded for this table.
  static int insert_ai_column(common::ObMySQLTransaction &trans,
                              const ObAiColumnSchema &ai_column);

  // Removes every AI column of `table_id`. Used when the table is dropped and
  // when an AI column is dropped from an existing table.
  static int delete_ai_column(common::ObMySQLTransaction &trans,
                              const uint64_t table_id,
                              const uint64_t column_id);
  static int delete_all_ai_columns(common::ObMySQLTransaction &trans,
                                   const uint64_t table_id);

  // Reads every AI column of `table_id`, in column_id order. `allocator` owns
  // the strings stored in the returned schemas.
  static int load_ai_columns(common::ObISQLClient &sql_client,
                             common::ObIAllocator &allocator,
                             const uint64_t table_id,
                             common::ObIArray<ObAiColumnSchema> &ai_columns);

  // Reads a single AI column. Returns OB_ENTRY_NOT_EXIST when the column is not
  // an AI column.
  static int load_ai_column(common::ObISQLClient &sql_client,
                            common::ObIAllocator &allocator,
                            const uint64_t table_id,
                            const uint64_t column_id,
                            ObAiColumnSchema &ai_column);

  // True when the table has at least one AI column. Used by the DDL and DML
  // paths to stay off the metadata tables for ordinary tables.
  static int has_ai_column(common::ObISQLClient &sql_client,
                           const uint64_t table_id,
                           bool &has_ai_column);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_PROXY_H_
