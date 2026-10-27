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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "share/ai_service/ob_ai_column_proxy.h"
#include "share/ai_service/ob_ai_column_struct.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
namespace oceanbase
{
namespace omt
{

static void escape_sql_string(const char *src, const int64_t src_len, ObSqlString &dst)
{
  for (int64_t i = 0; i < src_len; ++i) {
    const char c = src[i];
    if (c == '\'') {
      dst.append("''");
    } else if (c == '\\') {
      dst.append("\\\\");
    } else {
      dst.append(&c, 1);
    }
  }
}

ObAiColumnWorkerTask::ObAiColumnWorkerTask()
  : inited_(false), is_running_(false)
{
}

int ObAiColumnWorkerTask::init()
{
  inited_ = true;
  is_running_ = false;
  return OB_SUCCESS;
}

void ObAiColumnWorkerTask::runTimerTask()
{
  if (inited_ && !is_running_) {
    is_running_ = true;
    (void) do_work();
    is_running_ = false;
  }
}

int ObAiColumnWorkerTask::do_work()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (OB_ISNULL(sql_proxy)) {
    return OB_SUCCESS;
  }

  ObSqlString select_sql;
  if (OB_FAIL(select_sql.assign_fmt(
      "SELECT table_id, column_id, rowkey_hash, request_id, rowkey, rowkey_predicate, state, retry_count "
      "FROM oceanbase.__all_ai_column_task "
      "WHERE state = 0 OR (state = 3 AND eligible_ts <= now(6)) OR (state = 1 AND lease_ts <= date_sub(now(6), interval 30 second)) "
      "LIMIT 16"))) {
    return ret;
  }

  struct TaskItem {
    uint64_t table_id_;
    uint64_t column_id_;
    uint64_t rowkey_hash_;
    uint64_t request_id_;
    char rowkey_[128];
    char rowkey_pred_[256];
    int64_t state_;
    int64_t retry_count_;
  };
  ObSEArray<TaskItem, 16> tasks;

  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = nullptr;
    if (OB_FAIL(sql_proxy->read(res, select_sql.ptr()))) {
    } else if (OB_ISNULL(result = res.get_result())) {
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          }
        } else {
          TaskItem item;
          ObString rk;
          ObString rk_pred;
          EXTRACT_INT_FIELD_MYSQL(*result, "table_id", item.table_id_, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", item.column_id_, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "rowkey_hash", item.rowkey_hash_, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "request_id", item.request_id_, uint64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey", rk);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", rk_pred);
          EXTRACT_INT_FIELD_MYSQL(*result, "state", item.state_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", item.retry_count_, int64_t);
          if (OB_SUCC(ret)) {
            snprintf(item.rowkey_, sizeof(item.rowkey_), "%.*s", rk.length(), rk.ptr());
            snprintf(item.rowkey_pred_, sizeof(item.rowkey_pred_), "%.*s", rk_pred.length(), rk_pred.ptr());
            tasks.push_back(item);
          }
        }
      }
    }
  }

  if (tasks.empty()) {
    return OB_SUCCESS;
  }

  for (int64_t i = 0; i < tasks.count(); ++i) {
    const TaskItem &item = tasks.at(i);
    ObSqlString lease_sql;
    int64_t affected_rows = 0;
    lease_sql.assign_fmt(
        "UPDATE oceanbase.__all_ai_column_task SET state = 1, lease_ts = now(6) "
        "WHERE table_id = %lu AND column_id = %lu AND rowkey_hash = %lu AND request_id = %lu "
        "AND (state IN (0, 3) OR (state = 1 AND lease_ts <= date_sub(now(6), interval 30 second)))",
        item.table_id_, item.column_id_, item.rowkey_hash_, item.request_id_);
    sql_proxy->write(lease_sql.ptr(), affected_rows);
  }

  for (int64_t i = 0; i < tasks.count(); ++i) {
    const TaskItem &item = tasks.at(i);
    ObArenaAllocator alloc;
    share::ObAiColumnSchema ai_schema;
    if (OB_FAIL(share::ObAiColumnProxy::load_ai_column(*sql_proxy, alloc, item.table_id_, item.column_id_, ai_schema))) {
      LOG_WARN("failed to load ai column schema", K(ret), K(item.table_id_), K(item.column_id_));
      continue;
    }

    char db_name[OB_MAX_DATABASE_NAME_LENGTH] = {0};
    char table_name[OB_MAX_TABLE_NAME_LENGTH] = {0};
    ObSqlString table_info_sql;
    table_info_sql.assign_fmt(
        "SELECT d.database_name, t.table_name "
        "FROM oceanbase.__all_table t JOIN oceanbase.__all_database d ON t.database_id = d.database_id "
        "WHERE t.table_id = %lu", item.table_id_);
    SMART_VAR(ObMySQLProxy::MySQLResult, t_res) {
      ObMySQLResult *res_ptr = nullptr;
      if (OB_SUCC(sql_proxy->read(t_res, table_info_sql.ptr())) && OB_NOT_NULL(res_ptr = t_res.get_result())) {
        if (OB_SUCC(res_ptr->next())) {
          ObString d_str, t_str;
          EXTRACT_VARCHAR_FIELD_MYSQL(*res_ptr, "database_name", d_str);
          EXTRACT_VARCHAR_FIELD_MYSQL(*res_ptr, "table_name", t_str);
          snprintf(db_name, sizeof(db_name), "%.*s", d_str.length(), d_str.ptr());
          snprintf(table_name, sizeof(table_name), "%.*s", t_str.length(), t_str.ptr());
        }
      }
    }
    if (db_name[0] == '\0' || table_name[0] == '\0') {
      continue;
    }

    ObSqlString eval_sql;
    eval_sql.assign_fmt("SELECT %.*s AS ai_res FROM `%s`.`%s` WHERE %s",
                        ai_schema.normalized_expr_.length(), ai_schema.normalized_expr_.ptr(),
                        db_name, table_name, item.rowkey_pred_);

    bool eval_succ = false;
    char res_buf[4096] = {0};

    SMART_VAR(ObMySQLProxy::MySQLResult, eval_res) {
      ObMySQLResult *res_ptr = nullptr;
      if (OB_SUCC(sql_proxy->read(eval_res, eval_sql.ptr())) && OB_NOT_NULL(res_ptr = eval_res.get_result())) {
        if (OB_SUCC(res_ptr->next())) {
          ObString s;
          EXTRACT_VARCHAR_FIELD_MYSQL(*res_ptr, "ai_res", s);
          if (OB_SUCC(ret)) {
            eval_succ = true;
            int64_t copy_len = std::min(static_cast<int64_t>(sizeof(res_buf) - 1), static_cast<int64_t>(s.length()));
            MEMCPY(res_buf, s.ptr(), copy_len);
            res_buf[copy_len] = '\0';
          }
        }
      }
    }

    if (!eval_succ) {
      int64_t new_retry = item.retry_count_ + 1;
      if (new_retry < ai_schema.max_retries_) {
        ObSqlString retry_sql;
        int64_t aff = 0;
        retry_sql.assign_fmt(
            "UPDATE oceanbase.__all_ai_column_task SET state = 3, retry_count = %ld, eligible_ts = date_add(now(6), interval 1 second) "
            "WHERE table_id = %lu AND column_id = %lu AND rowkey_hash = %lu AND request_id = %lu",
            new_retry, item.table_id_, item.column_id_, item.rowkey_hash_, item.request_id_);
        sql_proxy->write(retry_sql.ptr(), aff);
      } else {
        char state_col_name[OB_MAX_COLUMN_NAME_LENGTH];
        snprintf(state_col_name, sizeof(state_col_name), "__ai_state_%.*s",
                 ai_schema.column_name_.length(), ai_schema.column_name_.ptr());
        ObSqlString fail_sql;
        int64_t aff = 0;
        fail_sql.assign_fmt("UPDATE `%s`.`%s` SET `%s` = %d WHERE %s",
                            db_name, table_name, state_col_name,
                            static_cast<int>(share::AI_COLUMN_TASK_FAILED),
                            item.rowkey_pred_);
        sql_proxy->write(fail_sql.ptr(), aff);

        ObSqlString del_sql;
        del_sql.assign_fmt(
            "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND column_id = %lu AND rowkey_hash = %lu AND request_id = %lu",
            item.table_id_, item.column_id_, item.rowkey_hash_, item.request_id_);
        sql_proxy->write(del_sql.ptr(), aff);
      }
    } else {
      bool is_stale = false;
      ObSqlString check_stale_sql;
      check_stale_sql.assign_fmt(
          "SELECT count(*) AS cnt FROM oceanbase.__all_ai_column_task "
          "WHERE table_id = %lu AND column_id = %lu AND rowkey_hash = %lu AND request_id = %lu",
          item.table_id_, item.column_id_, item.rowkey_hash_, item.request_id_);
      SMART_VAR(ObMySQLProxy::MySQLResult, c_res) {
        ObMySQLResult *res_ptr = nullptr;
        int64_t cnt = 0;
        if (OB_SUCC(sql_proxy->read(c_res, check_stale_sql.ptr())) && OB_NOT_NULL(res_ptr = c_res.get_result())) {
          if (OB_SUCC(res_ptr->next())) {
            EXTRACT_INT_FIELD_MYSQL(*res_ptr, "cnt", cnt, int64_t);
            if (cnt == 0) {
              is_stale = true;
            }
          }
        }
      }

      if (is_stale) {
        LOG_INFO("late old ai result dropped", K(item.table_id_), K(item.column_id_), K(item.request_id_));
        continue;
      }

      ObSqlString escaped_res;
      escape_sql_string(res_buf, strlen(res_buf), escaped_res);
      ObSqlString save_task_sql;
      int64_t aff = 0;
      save_task_sql.assign_fmt(
          "UPDATE oceanbase.__all_ai_column_task SET state = 2, request = '%.*s' "
          "WHERE table_id = %lu AND column_id = %lu AND rowkey_hash = %lu AND request_id = %lu",
          static_cast<int32_t>(escaped_res.length()), escaped_res.ptr(),
          item.table_id_, item.column_id_, item.rowkey_hash_, item.request_id_);
      sql_proxy->write(save_task_sql.ptr(), aff);

      int64_t unready_count = 0;
      ObSqlString unready_sql;
      unready_sql.assign_fmt(
          "SELECT count(*) AS cnt FROM oceanbase.__all_ai_column_task "
          "WHERE table_id = %lu AND rowkey_hash = %lu AND request_id = %lu AND state != 2",
          item.table_id_, item.rowkey_hash_, item.request_id_);
      SMART_VAR(ObMySQLProxy::MySQLResult, u_res) {
        ObMySQLResult *res_ptr = nullptr;
        if (OB_SUCC(sql_proxy->read(u_res, unready_sql.ptr())) && OB_NOT_NULL(res_ptr = u_res.get_result())) {
          if (OB_SUCC(res_ptr->next())) {
            EXTRACT_INT_FIELD_MYSQL(*res_ptr, "cnt", unready_count, int64_t);
          }
        }
      }

      if (unready_count == 0) {
        ObSqlString ready_tasks_sql;
        ready_tasks_sql.assign_fmt(
            "SELECT t.column_id, t.request, c.column_name "
            "FROM oceanbase.__all_ai_column_task t JOIN oceanbase.__all_ai_column c "
            "  ON t.table_id = c.table_id AND t.column_id = c.column_id "
            "WHERE t.table_id = %lu AND t.rowkey_hash = %lu AND t.request_id = %lu",
            item.table_id_, item.rowkey_hash_, item.request_id_);

        ObSqlString update_row_sql;
        update_row_sql.assign_fmt("UPDATE `%s`.`%s` SET ", db_name, table_name);

        bool first_set = true;
        SMART_VAR(ObMySQLProxy::MySQLResult, r_res) {
          ObMySQLResult *res_ptr = nullptr;
          if (OB_SUCC(sql_proxy->read(r_res, ready_tasks_sql.ptr())) && OB_NOT_NULL(res_ptr = r_res.get_result())) {
            while (OB_SUCC(res_ptr->next())) {
              uint64_t cid = 0;
              ObString col_n;
              ObString result_val;
              EXTRACT_INT_FIELD_MYSQL(*res_ptr, "column_id", cid, uint64_t);
              EXTRACT_VARCHAR_FIELD_MYSQL(*res_ptr, "column_name", col_n);
              EXTRACT_VARCHAR_FIELD_MYSQL(*res_ptr, "request", result_val);
              if (OB_SUCC(ret)) {
                if (!first_set) {
                  update_row_sql.append_fmt(", ");
                }
                first_set = false;
                char s_col[OB_MAX_COLUMN_NAME_LENGTH];
                snprintf(s_col, sizeof(s_col), "__ai_state_%.*s", col_n.length(), col_n.ptr());
                ObSqlString escaped_val;
                escape_sql_string(result_val.ptr(), result_val.length(), escaped_val);
                update_row_sql.append_fmt("`%.*s` = '%.*s', `%s` = %d",
                                          col_n.length(), col_n.ptr(),
                                          static_cast<int32_t>(escaped_val.length()), escaped_val.ptr(),
                                          s_col, static_cast<int>(share::AI_COLUMN_TASK_READY));
              }
            }
          }
        }

        if (!first_set) {
          update_row_sql.append_fmt(" WHERE %s", item.rowkey_pred_);
          int64_t aff_rows = 0;
          sql_proxy->write(update_row_sql.ptr(), aff_rows);

          ObSqlString del_done_sql;
          del_done_sql.assign_fmt(
              "DELETE FROM oceanbase.__all_ai_column_task "
              "WHERE table_id = %lu AND rowkey_hash = %lu AND request_id = %lu",
              item.table_id_, item.rowkey_hash_, item.request_id_);
          sql_proxy->write(del_done_sql.ptr(), aff_rows);
        }
      }
    }
  }
  return ret;
}

ObAiService::ObAiService()
  : is_inited_(false)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(timer_.init("AiColWorker", common::ObMemAttr("AiColWorker")))) {
    LOG_WARN("fail to init timer", K(ret));
  } else if (OB_FAIL(worker_task_.init())) {
    LOG_WARN("fail to init worker task", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(timer_.schedule(worker_task_, 100000 /* 100ms */, true))) {
    LOG_WARN("fail to schedule ai column worker task", K(ret));
  }
  return ret;
}

void ObAiService::stop()
{
  if (timer_.inited()) {
    timer_.stop();
  }
}

void ObAiService::wait()
{
  if (timer_.inited()) {
    timer_.wait();
  }
}

void ObAiService::destroy()
{
  if (timer_.inited()) {
    timer_.destroy();
  }
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
