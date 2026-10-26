/**
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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_READER_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_READER_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_iarray.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "share/ob_define.h"
#include "sql/resolver/expr/ob_raw_expr.h"

namespace oceanbase
{
namespace sql
{

class ObFileTableColumn;
class ObFileTableSchema;
class ObFileTableReader;

// The physical formats a file table can read. The value is derived from the
// table function name (read_csv / read_json / read_ndjson / read_parquet) so
// that an unsupported name can be rejected before any file is touched.
enum ObFileTableFormat
{
  FILE_TABLE_FORMAT_INVALID = 0,
  FILE_TABLE_FORMAT_CSV = 1,
  FILE_TABLE_FORMAT_JSON = 2,
  FILE_TABLE_FORMAT_NDJSON = 3,
  FILE_TABLE_FORMAT_PARQUET = 4,
};

// One inferred column of a file table. `type_` is the column type the rows were
// coerced to, which is also what `DESCRIBE SELECT ... FROM read_csv(...)`
// reports.
struct ObFileTableColumn
{
  ObFileTableColumn()
    : name_(), type_(), nullable_(true) {}
  ObString name_;
  ObExprResType type_;
  bool nullable_;
  TO_STRING_KV(K_(name), K_(type), K_(nullable));
};

// Schema inference result shared by the resolver and the readers. `columns_`
// is ordered as the file exposes it; duplicate CSV headers are disambiguated
// while the schema is built, so `columns_` never holds two equal names.
class ObFileTableSchema
{
public:
  ObFileTableSchema() : columns_() {}
  int64_t count() const { return columns_.count(); }
  const ObFileTableColumn &at(const int64_t idx) const { return columns_.at(idx); }
  common::ObIArray<ObFileTableColumn> &columns() { return columns_; }
  const common::ObIArray<ObFileTableColumn> &columns() const { return columns_; }
  TO_STRING_KV(K_(columns));
private:
  common::ObSEArray<ObFileTableColumn, 8> columns_;
};

// Options accepted by the table functions. Anything not listed here (notably a
// `format` other than the supported literals) is rejected by the resolver.
struct ObFileTableOptions
{
  ObFileTableOptions()
    : format_(FILE_TABLE_FORMAT_INVALID),
      sample_size_(-1),
      header_(true),
      field_delimiter_(','),
      line_delimiter_('\n'),
      quote_('"'),
      escape_('\\'),
      skip_header_(0),
      nullstr_() {}
  ObFileTableFormat format_;
  int64_t sample_size_;      // -1 means "infer from the whole file"
  bool header_;              // CSV: first line holds the column names
  char field_delimiter_;
  char line_delimiter_;
  char quote_;
  char escape_;
  int64_t skip_header_;
  ObString nullstr_;         // field value mapped to SQL NULL
  TO_STRING_KV(K_(format), K_(sample_size), K_(header), K_(field_delimiter),
               K_(line_delimiter), K_(quote), K_(escape), K_(skip_header), K_(nullstr));
};

// Resolves a table function name to its physical format. Returns
// FILE_TABLE_FORMAT_INVALID for names that are not file table functions.
ObFileTableFormat ob_file_table_format_from_name(const ObString &func_name);

// True when `func_name` names one of the file table functions. Used to decide
// whether a `name(...)` in a FROM clause is a file table rather than a stored
// function table.
bool ob_is_file_table_func(const ObString &func_name);

// True when `func_name` is `glob`, which returns the matched file names rather
// than file contents.
bool ob_is_file_table_glob(const ObString &func_name);

// Infers the schema and reads every row of the file(s) named by `path`.
//
// `path` may contain glob wildcards; every matching file is read in name order.
// Rows are appended to `objs` row-major (row_cnt * column_cnt entries), each
// already coerced to the type recorded in `schema`. On success `row_cnt` holds
// the number of rows appended.
//
// The caller is responsible for having validated `path` against
// secure_file_priv; this class only opens paths it is given.
class ObFileTableReader
{
public:
  ObFileTableReader(common::ObIAllocator &allocator,
                    const ObFileTableOptions &options,
                    ObFileTableSchema &schema);
  ~ObFileTableReader() {}

  // Expands `path` as a glob, reads every match and appends rows to `objs`.
  int read(const ObString &path,
           common::ObIArray<common::ObObjParam> &objs,
           int64_t &row_cnt);

  // Expands `path` and appends the matched file names to `files`. Used by
  // glob(), which returns the file list rather than file contents.
  int list(const ObString &path, common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt);

private:
  int read_one_file(const ObString &path,
                    common::ObIArray<common::ObObjParam> &objs,
                    int64_t &row_cnt);
  int read_csv(const char *data, const int64_t len,
               common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt);
  int read_json(const char *data, const int64_t len, const bool newline_delimited,
                common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt);
  int read_parquet(const char *data, const int64_t len,
                   common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt);

  // Builds `schema_` from already-parsed raw text rows. Called by the readers
  // before any row is coerced.
  int infer_schema_from_csv(const char *data, const int64_t len);
  int infer_schema_from_json(const char *data, const int64_t len, const bool newline_delimited);

  int append_value(common::ObIArray<common::ObObjParam> &objs,
                   const ObFileTableColumn &column, const char *ptr, const int64_t len);
  int append_json_value(common::ObIArray<common::ObObjParam> &objs,
                        const ObFileTableColumn &column, const common::ObJsonNode &value);

  common::ObIAllocator &allocator_;
  const ObFileTableOptions &options_;
  ObFileTableSchema &schema_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_READER_H_
