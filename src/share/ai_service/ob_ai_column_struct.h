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

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_STRUCT_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_STRUCT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace share
{

// The lifecycle of one (row, AI column) task. The numeric values are persisted
// in __all_ai_column_task.state, so they must not be renumbered.
enum ObAiColumnTaskState
{
  AI_COLUMN_TASK_PENDING = 0,   // waiting for a worker lease
  AI_COLUMN_TASK_RUNNING = 1,   // leased by a worker; a stale lease is reclaimable
  AI_COLUMN_TASK_READY = 2,     // the model call succeeded; the result is applied
  AI_COLUMN_TASK_FAILED = 3,    // the last attempt failed; may still be retried
};

// `sync_mode` is accepted for compatibility with the declared syntax; the
// implementation always runs asynchronously, which is what the `async` value
// asks for and what the synchronous value degrades to under load.
enum ObAiColumnSyncMode
{
  AI_COLUMN_SYNC_ASYNC = 0,
  AI_COLUMN_SYNC_SYNC = 1,
};

// One row of __all_ai_column. `column_id` is the user-visible result column;
// the three hidden ids belong to the columns that carry the state machine, the
// rendered request and the response schema.
struct ObAiColumnSchema
{
  ObAiColumnSchema()
    : table_id_(OB_INVALID_ID),
      column_id_(OB_INVALID_ID),
      column_name_(),
      max_retries_(0),
      sync_mode_(AI_COLUMN_SYNC_ASYNC),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      result_spec_column_id_(OB_INVALID_ID),
      normalized_expr_(),
      column_flags_(0) {}

  void reset() { *this = ObAiColumnSchema(); }

  uint64_t table_id_;
  uint64_t column_id_;
  ObString column_name_;
  int64_t max_retries_;
  int64_t sync_mode_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  ObString normalized_expr_;
  int64_t column_flags_;

  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(max_retries),
               K_(sync_mode), K_(state_column_id), K_(request_column_id),
               K_(result_spec_column_id), K_(normalized_expr), K_(column_flags));
};

// One row of __all_ai_column_task. `rowkey_hash_` is the primary key's hash and
// only serves as the last part of the inner table's rowkey; `rowkey_predicate_`
// is the human-readable form the retry statement and the tests match on.
struct ObAiColumnTask
{
  ObAiColumnTask()
    : table_id_(OB_INVALID_ID),
      column_id_(OB_INVALID_ID),
      rowkey_hash_(0),
      request_id_(0),
      rowkey_(),
      rowkey_predicate_(),
      state_(AI_COLUMN_TASK_PENDING),
      retry_count_(0),
      eligible_ts_(0),
      lease_ts_(0),
      request_(),
      ret_code_(0),
      trace_id_() {}

  void reset() { *this = ObAiColumnTask(); }

  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t rowkey_hash_;
  uint64_t request_id_;
  ObString rowkey_;
  ObString rowkey_predicate_;
  int64_t state_;
  int64_t retry_count_;
  int64_t eligible_ts_;
  int64_t lease_ts_;
  ObString request_;
  int64_t ret_code_;
  ObString trace_id_;

  TO_STRING_KV(K_(table_id), K_(column_id), K_(rowkey_hash), K_(request_id),
               K_(rowkey), K_(rowkey_predicate), K_(state), K_(retry_count),
               K_(eligible_ts), K_(lease_ts), K_(ret_code), K_(trace_id));
};

// Names of the hidden columns every AI column brings along. They are prefixed so
// that they cannot collide with a user column, and are created hidden so that
// `SELECT *` does not expose them.
const char *const OB_AI_COLUMN_STATE_COLUMN_NAME = "__ai_state";
const char *const OB_AI_COLUMN_REQUEST_COLUMN_NAME = "__ai_request";
const char *const OB_AI_COLUMN_RESULT_SPEC_COLUMN_NAME = "__ai_result_spec";

// The AI column as the DDL parser produced it, before the hidden columns and
// the table id exist. It travels from the resolver to the DDL service inside
// ObCreateTableArg / ObAlterTableArg.
struct ObAiColumnDefinition
{
  OB_UNIS_VERSION(1);
public:
  ObAiColumnDefinition()
    : column_name_(),
      column_id_(OB_INVALID_ID),
      normalized_expr_(),
      max_retries_(0),
      sync_mode_(AI_COLUMN_SYNC_ASYNC),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      result_spec_column_id_(OB_INVALID_ID) {}

  void reset() { *this = ObAiColumnDefinition(); }

  TO_STRING_KV(K_(column_name), K_(column_id), K_(normalized_expr), K_(max_retries),
               K_(sync_mode), K_(state_column_id), K_(request_column_id),
               K_(result_spec_column_id));

  ObString column_name_;
  uint64_t column_id_;
  ObString normalized_expr_;
  int64_t max_retries_;
  int64_t sync_mode_;
  // Assigned by the DDL service once the hidden columns have ids.
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_COLUMN_STRUCT_H_
