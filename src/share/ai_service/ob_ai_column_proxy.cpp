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

#define USING_LOG_PREFIX SHARE

#include "share/ai_service/ob_ai_column_proxy.h"

#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
namespace share
{

OB_SERIALIZE_MEMBER(ObAiColumnDefinition, column_name_, column_id_, normalized_expr_,
                    max_retries_, sync_mode_, state_column_id_,
                    request_column_id_, result_spec_column_id_);

int ObAiColumnProxy::insert_ai_column(ObMySQLTransaction &trans,
                                      const ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("table_id", ai_column.table_id_))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", ai_column.column_id_))) {
  } else if (OB_FAIL(sql.add_column("column_name", ObHexEscapeSqlStr(ai_column.column_name_)))) {
  } else if (OB_FAIL(sql.add_column("max_retries", ai_column.max_retries_))) {
  } else if (OB_FAIL(sql.add_column("sync_mode",
                                    ObHexEscapeSqlStr(AI_COLUMN_SYNC_SYNC == ai_column.sync_mode_
                                        ? ObString::make_string("sync")
                                        : ObString::make_string("async"))))) {
  } else if (OB_FAIL(sql.add_column("state_column_id", ai_column.state_column_id_))) {
  } else if (OB_FAIL(sql.add_column("request_column_id", ai_column.request_column_id_))) {
  } else if (OB_FAIL(sql.add_column("result_spec_column_id", ai_column.result_spec_column_id_))) {
  } else if (OB_FAIL(sql.add_column("normalized_expr", ObHexEscapeSqlStr(ai_column.normalized_expr_)))) {
  } else if (OB_FAIL(sql.add_column("column_flags", ai_column.column_flags_))) {
  } else if (OB_FAIL(sql.splice_insert_sql(OB_ALL_AI_COLUMN_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", K(ret), K(affected_rows));
  }

  if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
    ret = OB_ENTRY_EXIST;
  }
  return ret;
}

int ObAiColumnProxy::delete_ai_column(ObMySQLTransaction &trans,
                                      const uint64_t table_id,
                                      const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu AND column_id = %lu",
                             OB_ALL_AI_COLUMN_TNAME, table_id, column_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

int ObAiColumnProxy::delete_all_ai_columns(ObMySQLTransaction &trans,
                                           const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                             OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

namespace
{

// Shared column list so that the single-row and multi-row readers cannot drift
// from the inner table's definition.
const char *const AI_COLUMN_SELECT_LIST =
    "table_id, column_id, column_name, max_retries, sync_mode, "
    "state_column_id, request_column_id, result_spec_column_id, "
    "normalized_expr, column_flags";

int build_ai_column(ObIAllocator &allocator, ObMySQLResult &result,
                    ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ObString column_name;
  ObString sync_mode;
  ObString normalized_expr;
  ai_column.reset();
  EXTRACT_INT_FIELD_MYSQL(result, "table_id", ai_column.table_id_, uint64_t);
  EXTRACT_INT_FIELD_MYSQL(result, "column_id", ai_column.column_id_, uint64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "column_name", column_name);
  EXTRACT_INT_FIELD_MYSQL(result, "max_retries", ai_column.max_retries_, int64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "sync_mode", sync_mode, true, false, ""/*unused*/);
  EXTRACT_INT_FIELD_MYSQL(result, "state_column_id", ai_column.state_column_id_, uint64_t);
  EXTRACT_INT_FIELD_MYSQL(result, "request_column_id", ai_column.request_column_id_, uint64_t);
  EXTRACT_INT_FIELD_MYSQL(result, "result_spec_column_id", ai_column.result_spec_column_id_, uint64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "normalized_expr", normalized_expr, true, false, ""/*unused*/);
  EXTRACT_INT_FIELD_MYSQL(result, "column_flags", ai_column.column_flags_, int64_t);

  if (OB_SUCC(ret)) {
    ai_column.sync_mode_ = (0 == sync_mode.case_compare("sync"))
        ? AI_COLUMN_SYNC_SYNC : AI_COLUMN_SYNC_ASYNC;
    if (OB_FAIL(ob_write_string(allocator, column_name, ai_column.column_name_))) {
    } else if (OB_FAIL(ob_write_string(allocator, normalized_expr,
                                       ai_column.normalized_expr_))) {
    }
  }
  return ret;
}

}  // namespace

int ObAiColumnProxy::load_ai_columns(ObISQLClient &sql_client,
                                     ObIAllocator &allocator,
                                     const uint64_t table_id,
                                     ObIArray<ObAiColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("SELECT %s FROM %s WHERE table_id = %lu ORDER BY column_id",
                             AI_COLUMN_SELECT_LIST, OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get result failed", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          ObAiColumnSchema ai_column;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("read next row failed", K(ret));
            }
          } else if (OB_FAIL(build_ai_column(allocator, *result, ai_column))) {
          } else if (OB_FAIL(ai_columns.push_back(ai_column))) {
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnProxy::load_ai_column(ObISQLClient &sql_client,
                                    ObIAllocator &allocator,
                                    const uint64_t table_id,
                                    const uint64_t column_id,
                                    ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
      "SELECT %s FROM %s WHERE table_id = %lu AND column_id = %lu",
      AI_COLUMN_SELECT_LIST, OB_ALL_AI_COLUMN_TNAME, table_id, column_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get result failed", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_ENTRY_NOT_EXIST;
        } else {
          LOG_WARN("read next row failed", K(ret));
        }
      } else if (OB_FAIL(build_ai_column(allocator, *result, ai_column))) {
      } else {
        int tmp_ret = OB_SUCCESS;
        if (OB_ITER_END != (tmp_ret = result->next())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get more row than one", K(ret), K(tmp_ret), K(sql));
        }
      }
    }
  }
  return ret;
}

int ObAiColumnProxy::has_ai_column(ObISQLClient &sql_client,
                                   const uint64_t table_id,
                                   bool &has_ai_column)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  has_ai_column = false;
  if (OB_FAIL(sql.assign_fmt("SELECT COUNT(*) AS cnt FROM %s WHERE table_id = %lu",
                             OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      int64_t count = 0;
      if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get result failed", K(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("read next row failed", K(ret));
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "cnt", count, int64_t);
        if (OB_SUCC(ret)) {
          has_ai_column = count > 0;
        }
      }
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
