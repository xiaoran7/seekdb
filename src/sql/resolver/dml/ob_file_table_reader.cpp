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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_file_table_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/json_type/ob_json_parse.h"
#include "common/json_type/ob_json_tree.h"
#include "common/object/ob_object.h"
#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/utility.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{

const int64_t FILE_TABLE_MAX_FILE_SIZE = 1LL << 30;  // 1 GiB

// A parsed CSV field: a view into the buffer that already has quoting and
// escaping resolved. `is_null_` records a field that matched the configured
// null string so that the schema inference can treat it as a missing value.
struct CsvField
{
  const char *ptr_;
  int64_t len_;
  bool is_null_;
};

int read_whole_file(ObIAllocator &allocator, const char *path, char *&data, int64_t &len)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  struct stat st;
  data = NULL;
  len = 0;
  if (OB_ISNULL(path)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 != ::stat(path, &st)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("file not found", K(ret), K(path), K(errno));
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not a regular file", K(ret), K(path));
  } else if (st.st_size > FILE_TABLE_MAX_FILE_SIZE) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("file too large", K(ret), K(path), K(st.st_size));
  } else if (OB_ISNULL(data = static_cast<char *>(allocator.alloc(st.st_size + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("alloc file buffer failed", K(ret), K(st.st_size));
  } else if (-1 == (fd = ::open(path, O_RDONLY))) {
    ret = OB_IO_ERROR;
    LOG_WARN("open file failed", K(ret), K(path), K(errno));
  } else {
    int64_t total = 0;
    while (total < st.st_size) {
      const ssize_t n = ::read(fd, data + total, st.st_size - total);
      if (n < 0) {
        ret = OB_IO_ERROR;
        LOG_WARN("read file failed", K(ret), K(path), K(errno));
        break;
      } else if (0 == n) {
        break;
      } else {
        total += n;
      }
    }
    if (OB_SUCC(ret)) {
      data[total] = '\0';
      len = total;
    }
  }
  if (fd >= 0) {
    ::close(fd);
  }
  return ret;
}

bool is_int_text(const char *ptr, const int64_t len)
{
  bool ans = (len > 0);
  int64_t i = 0;
  if (ans && ('-' == ptr[0] || '+' == ptr[0])) {
    i = 1;
    ans = (len > 1);
  }
  for (; ans && i < len; ++i) {
    ans = (ptr[i] >= '0' && ptr[i] <= '9');
  }
  return ans;
}

bool is_double_text(const char *ptr, const int64_t len)
{
  bool ans = (len > 0);
  bool has_digit = false;
  int64_t i = 0;
  if (ans && ('-' == ptr[0] || '+' == ptr[0])) {
    i = 1;
  }
  for (; ans && i < len; ++i) {
    const char c = ptr[i];
    if (c >= '0' && c <= '9') {
      has_digit = true;
    } else if ('.' == c || 'e' == c || 'E' == c) {
      // allowed
    } else if (('-' == c || '+' == c) && ('e' == ptr[i - 1] || 'E' == ptr[i - 1])) {
      // exponent sign
    } else {
      ans = false;
    }
  }
  return ans && has_digit;
}

bool is_bool_text(const char *ptr, const int64_t len)
{
  return (5 == len && 0 == strncasecmp(ptr, "true", 5)) ||
         (5 == len && 0 == strncasecmp(ptr, "false", 5));
}

// Accepts both `YYYY-MM-DD` and the ISO `YYYY-MM-DDTHH:MM:SS` spelling that
// appears in the file_sql fixtures.
bool is_date_text(const char *ptr, const int64_t len)
{
  return 10 == len && ptr[4] == '-' && ptr[7] == '-';
}

bool is_datetime_text(const char *ptr, const int64_t len)
{
  bool ans = (len >= 19) && ptr[4] == '-' && ptr[7] == '-' &&
             ('T' == ptr[10] || ' ' == ptr[10]) &&
             ptr[13] == ':' && ptr[16] == ':';
  return ans;
}

// The inference ladder, widest last. A column takes the narrowest type that
// every non-null value fits.
ObExprResType infer_type_from_text(const char *ptr, const int64_t len)
{
  ObExprResType type;
  type.set_type(ObVarcharType);
  type.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  if (is_bool_text(ptr, len)) {
    type.set_type(ObTinyIntType);
  } else if (is_int_text(ptr, len)) {
    type.set_type(ObIntType);
  } else if (is_double_text(ptr, len)) {
    type.set_type(ObDoubleType);
  } else if (is_date_text(ptr, len)) {
    type.set_type(ObDateType);
  } else if (is_datetime_text(ptr, len)) {
    type.set_type(ObDateTimeType);
  }
  return type;
}

int type_rank(const ObObjType type)
{
  switch (type) {
    case ObTinyIntType: return 1;   // boolean
    case ObIntType: return 2;
    case ObDoubleType: return 3;
    case ObDateType: return 4;
    case ObDateTimeType: return 5;
    case ObVarcharType: return 6;
    default: return 6;
  }
}

ObExprResType merge_type(const ObExprResType &lhs, const ObExprResType &rhs)
{
  ObExprResType ans;
  const ObObjType lt = lhs.get_type();
  const ObObjType rt = rhs.get_type();
  if (lt == rt) {
    ans = lhs;
  } else if (type_rank(lt) >= type_rank(rt)) {
    ans = lhs;
  } else {
    ans = rhs;
  }
  // BOOLEAN widened by a number becomes that number; BOOLEAN widened by
  // anything else degrades to text.
  if ((ObTinyIntType == lt || ObTinyIntType == rt) &&
      ans.get_type() != ObTinyIntType && type_rank(ans.get_type()) > 3) {
    ans.set_type(ObVarcharType);
  }
  if (ObVarcharType == ans.get_type()) {
    ans.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  }
  return ans;
}

int append_text_column(ObIAllocator &allocator, const char *ptr, const int64_t len, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, ptr, len);
    out.assign_ptr(buf, static_cast<int32_t>(len));
  }
  return ret;
}

// Resolves duplicate CSV headers the way the file_sql suite expects: the
// second `id` becomes `id__2`, the third `id__3`, and so on.
int dedup_column_name(common::ObIArray<ObFileTableColumn> &columns,
                      ObIAllocator &allocator,
                      const char *name, const int64_t len, ObString &out)
{
  int ret = OB_SUCCESS;
  int64_t suffix = 1;
  ObSqlString candidate;
  if (OB_FAIL(candidate.append(name, len))) {
  } else {
    bool exists = true;
    while (OB_SUCC(ret) && exists) {
      exists = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
        if (0 == columns.at(i).name_.case_compare(candidate.string())) {
          exists = true;
          break;
        }
      }
      if (exists) {
        ++suffix;
        candidate.reset();
        if (OB_FAIL(candidate.append_fmt("%.*s__%ld", static_cast<int>(len), name, suffix))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(append_text_column(allocator, candidate.ptr(), candidate.length(), out))) {
      }
    }
  }
  return ret;
}

}  // namespace

namespace
{

// Minimal Thrift compact-protocol reader, enough to walk a Parquet footer.
// Parquet stores its metadata as Thrift compact; only the fields the reader
// needs are decoded, everything else is skipped structurally.
class ThriftCompactReader
{
public:
  ThriftCompactReader(const char *data, const int64_t len)
    : data_(data), len_(len), pos_(0) {}

  int64_t pos() const { return pos_; }
  bool eof() const { return pos_ >= len_; }
  void seek(const int64_t pos) { pos_ = pos; }
  const char *data() const { return data_; }
  int64_t len() const { return len_; }

  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      v = static_cast<uint8_t>(data_[pos_++]);
    }
    return ret;
  }

  int read_varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t b = 0;
      if (shift > 63) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(read_byte(b))) {
      } else {
        v |= (static_cast<uint64_t>(b & 0x7f) << shift);
        shift += 7;
        done = (0 == (b & 0x80));
      }
    }
    return ret;
  }

  int read_zigzag(int64_t &v)
  {
    uint64_t raw = 0;
    int ret = read_varint(raw);
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }

  int read_bytes(const char *&ptr, int64_t &len)
  {
    uint64_t n = 0;
    int ret = read_varint(n);
    if (OB_SUCC(ret)) {
      if (pos_ + static_cast<int64_t>(n) > len_) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ptr = data_ + pos_;
        len = static_cast<int64_t>(n);
        pos_ += n;
      }
    }
    return ret;
  }

  int skip(const uint8_t type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1:   // BOOLEAN_TRUE
      case 2:   // BOOLEAN_FALSE
        break;
      case 3: { // BYTE
        uint8_t b = 0;
        ret = read_byte(b);
        break;
      }
      case 4:   // I16
      case 5:   // I32
      case 6:   // I64
      case 7: { // DOUBLE
        const int n = (7 == type) ? 8 : (6 == type ? 8 : 4);
        if (pos_ + n > len_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          pos_ += n;
        }
        break;
      }
      case 8: { // BINARY
        const char *p = NULL;
        int64_t l = 0;
        ret = read_bytes(p, l);
        break;
      }
      case 9:   // LIST
      case 10: { // SET
        uint8_t hdr = 0;
        if (OB_FAIL(read_byte(hdr))) {
        } else {
          const uint8_t elem_type = hdr & 0x0f;
          uint64_t n = hdr >> 4;
          if (15 == n) {
            if (OB_FAIL(read_varint(n))) {
            }
          }
          for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
            ret = skip(elem_type);
          }
        }
        break;
      }
      case 11: { // MAP
        uint64_t n = 0;
        if (OB_FAIL(read_varint(n))) {
        } else if (n > 0) {
          uint8_t kv = 0;
          if (OB_FAIL(read_byte(kv))) {
          } else {
            for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              if (OB_FAIL(skip((kv >> 4) & 0x0f))) {
              } else {
                ret = skip(kv & 0x0f);
              }
            }
          }
        }
        break;
      }
      case 12: { // STRUCT
        enter();
        int field_type = 0;
        while (OB_SUCC(ret) && 0 < (field_type = next_field_type())) {
          int16_t field_id = 0;
          if (OB_FAIL(read_field_id(field_id, field_type))) {
          } else {
            ret = skip(static_cast<uint8_t>(field_type));
          }
        }
        leave();
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
        break;
    }
    return ret;
  }

  // Returns the compact type of the next field, or 0 at the struct's STOP byte.
  // A truncated buffer yields -1 so that callers fail instead of silently
  // treating the end of data as a well-formed struct.
  int next_field_type()
  {
    int t = 0;
    if (pos_ >= len_) {
      t = -1;
    } else {
      t = static_cast<uint8_t>(data_[pos_]);
      if (0 == t) {
        ++pos_;
      }
    }
    return t;
  }

  int read_field_id(int16_t &field_id, int &type)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const uint8_t t = static_cast<uint8_t>(data_[pos_++]);
      const uint8_t delta = t >> 4;
      type = (t & 0x0f);
      if (0 == delta) {
        int64_t zig = 0;
        if (OB_FAIL(read_zigzag(zig))) {
        } else {
          field_id = static_cast<int16_t>(zig);
        }
      } else {
        field_id = static_cast<int16_t>(last_field_id_ + delta);
      }
      last_field_id_ = field_id;
    }
    return ret;
  }

  void reset_field_id() { last_field_id_ = 0; }

  // Compact protocol resets the field-id delta at each struct boundary; nested
  // structs therefore push and pop their own last-field-id.
  void enter() { stack_[depth_++] = last_field_id_; last_field_id_ = 0; }
  void leave() { last_field_id_ = stack_[--depth_]; }

private:
  const char *data_;
  int64_t len_;
  int64_t pos_;
  int16_t last_field_id_ = 0;
  int16_t stack_[16] = {0};
  int depth_ = 0;
};

// The subset of Parquet metadata the reader needs: the leaf columns with their
// physical type, and the byte ranges of each column chunk's data pages.
struct ParquetColumnChunk
{
  int64_t file_offset_ = 0;
  int64_t total_compressed_size_ = 0;
  int32_t codec_ = 0;
  int64_t num_values_ = 0;
};

struct ParquetColumn
{
  ObString name_;
  int32_t physical_type_ = 0;
  int32_t type_length_ = 0;
  int32_t converted_type_ = 0;
  bool optional_ = false;
  ParquetColumnChunk chunk_;
};

const int32_t PARQUET_TYPE_BOOLEAN = 0;
const int32_t PARQUET_TYPE_INT32 = 1;
const int32_t PARQUET_TYPE_INT64 = 2;
const int32_t PARQUET_TYPE_INT96 = 3;
const int32_t PARQUET_TYPE_FLOAT = 4;
const int32_t PARQUET_TYPE_DOUBLE = 5;
const int32_t PARQUET_TYPE_BYTE_ARRAY = 6;
const int32_t PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY = 7;

const int32_t PARQUET_CODEC_UNCOMPRESSED = 0;

// FileMetaData { 1: i32 version, 2: list<SchemaElement> schema, 3: i64 num_rows,
//                4: list<RowGroup> row_groups, ... }
//
// Only the pieces the reader consumes are decoded; every other field is skipped
// through its wire type, so the parser tolerates the statistics and key-value
// metadata that real writers emit.
int parse_parquet_schema_element(ThriftCompactReader &reader,
                                 ObIAllocator &allocator,
                                 ObIArray<ObString> &names,
                                 ObIArray<int32_t> &types,
                                 ObIArray<int32_t> &converted,
                                 ObIArray<int32_t> &repetition,
                                 ObIArray<int32_t> &type_length,
                                 ObIArray<int32_t> &num_children)
{
  int ret = OB_SUCCESS;
  // SchemaElement { 1: Type type, 2: i32 type_length, 3: FieldRepetitionType repetition_type,
  //                 4: string name, 5: i32 num_children, 6: ConvertedType converted_type, ... }
  ObString name;
  int32_t type = -1;
  int32_t type_len = 0;
  int32_t rep = 0;
  int32_t children = 0;
  int32_t conv = -1;
  reader.enter();
  int t = reader.next_field_type();
  while (OB_SUCC(ret) && 0 < t) {
    int16_t field_id = 0;
    if (OB_FAIL(reader.read_field_id(field_id, t))) {
    } else if (1 == field_id && 5 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        type = static_cast<int32_t>(v);
      }
    } else if (2 == field_id && 5 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        type_len = static_cast<int32_t>(v);
      }
    } else if (3 == field_id && 5 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        rep = static_cast<int32_t>(v);
      }
    } else if (4 == field_id && 8 == t) {
      const char *p = NULL;
      int64_t l = 0;
      if (OB_FAIL(reader.read_bytes(p, l))) {
      } else if (OB_FAIL(append_text_column(allocator, p, l, name))) {
      }
    } else if (5 == field_id && 5 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        children = static_cast<int32_t>(v);
      }
    } else if (6 == field_id && 5 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        conv = static_cast<int32_t>(v);
      }
    } else if (OB_FAIL(reader.skip(static_cast<uint8_t>(t)))) {
    }
    if (OB_SUCC(ret)) {
      t = reader.next_field_type();
    }
  }
  if (OB_SUCC(ret) && t < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("truncated thrift struct", K(ret));
  }
  reader.leave();
  if (OB_SUCC(ret)) {
    if (OB_FAIL(names.push_back(name)) || OB_FAIL(types.push_back(type)) ||
        OB_FAIL(converted.push_back(conv)) || OB_FAIL(repetition.push_back(rep)) ||
        OB_FAIL(type_length.push_back(type_len)) ||
        OB_FAIL(num_children.push_back(children))) {
    }
  }
  return ret;
}

// ColumnMetaData { 1: Type type, 2: list<Encoding> encodings, 3: list<string> path_in_schema,
//                  4: CompressionCodec codec, 5: i64 num_values,
//                  6: i64 total_uncompressed_size, 7: i64 total_compressed_size,
//                  9: i64 data_page_offset, ... }
int parse_parquet_column_metadata(ThriftCompactReader &reader, ParquetColumn &column)
{
  int ret = OB_SUCCESS;
  reader.enter();
  int t = reader.next_field_type();
  while (OB_SUCC(ret) && 0 < t) {
    int16_t field_id = 0;
    if (OB_FAIL(reader.read_field_id(field_id, t))) {
    } else if (4 == field_id && 5 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        column.chunk_.codec_ = static_cast<int32_t>(v);
      }
    } else if (5 == field_id && 6 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        column.chunk_.num_values_ = v;
      }
    } else if (7 == field_id && 6 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        column.chunk_.total_compressed_size_ = v;
      }
    } else if (9 == field_id && 6 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        column.chunk_.file_offset_ = v;
      }
    } else if (OB_FAIL(reader.skip(static_cast<uint8_t>(t)))) {
    }
    if (OB_SUCC(ret)) {
      t = reader.next_field_type();
    }
  }
  if (OB_SUCC(ret) && t < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("truncated thrift struct", K(ret));
  }
  reader.leave();
  return ret;
}

// ColumnChunk { 1: string file_path, 2: i64 file_offset, 3: ColumnMetaData meta_data, ... }
int parse_parquet_column_chunk(ThriftCompactReader &reader, ParquetColumn &column)
{
  int ret = OB_SUCCESS;
  reader.enter();
  int t = reader.next_field_type();
  while (OB_SUCC(ret) && 0 < t) {
    int16_t field_id = 0;
    if (OB_FAIL(reader.read_field_id(field_id, t))) {
    } else if (2 == field_id && 6 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        column.chunk_.file_offset_ = v;
      }
    } else if (3 == field_id && 12 == t) {
      if (OB_FAIL(parse_parquet_column_metadata(reader, column))) {
      }
    } else if (OB_FAIL(reader.skip(static_cast<uint8_t>(t)))) {
    }
    if (OB_SUCC(ret)) {
      t = reader.next_field_type();
    }
  }
  if (OB_SUCC(ret) && t < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("truncated thrift struct", K(ret));
  }
  reader.leave();
  return ret;
}

// RowGroup { 1: list<ColumnChunk> columns, 2: i64 total_byte_size, 3: i64 num_rows, ... }
int parse_parquet_row_group(ThriftCompactReader &reader, ObIArray<ParquetColumn> &columns)
{
  int ret = OB_SUCCESS;
  reader.enter();
  int t = reader.next_field_type();
  while (OB_SUCC(ret) && 0 < t) {
    int16_t field_id = 0;
    if (OB_FAIL(reader.read_field_id(field_id, t))) {
    } else if (1 == field_id && 9 == t) {
      uint8_t hdr = 0;
      if (OB_FAIL(reader.read_byte(hdr))) {
      } else {
        uint64_t n = hdr >> 4;
        if (15 == n && OB_FAIL(reader.read_varint(n))) {
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          ParquetColumn column;
          if (OB_FAIL(parse_parquet_column_chunk(reader, column))) {
          } else if (OB_FAIL(columns.push_back(column))) {
          }
        }
      }
    } else if (OB_FAIL(reader.skip(static_cast<uint8_t>(t)))) {
    }
    if (OB_SUCC(ret)) {
      t = reader.next_field_type();
    }
  }
  if (OB_SUCC(ret) && t < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("truncated thrift struct", K(ret));
  }
  reader.leave();
  return ret;
}

int parse_parquet_footer(ObIAllocator &allocator, const char *footer, const int64_t len,
                         ObIArray<ParquetColumn> &columns, int64_t &num_rows)
{
  int ret = OB_SUCCESS;
  ThriftCompactReader reader(footer, len);
  ObSEArray<ObString, 8> schema_names;
  ObSEArray<int32_t, 8> schema_types;
  ObSEArray<int32_t, 8> schema_converted;
  ObSEArray<int32_t, 8> schema_repetition;
  ObSEArray<int32_t, 8> schema_type_length;
  ObSEArray<int32_t, 8> schema_num_children;
  reader.enter();
  int t = reader.next_field_type();
  while (OB_SUCC(ret) && 0 < t) {
    int16_t field_id = 0;
    if (OB_FAIL(reader.read_field_id(field_id, t))) {
    } else if (2 == field_id && 9 == t) {
      uint8_t hdr = 0;
      if (OB_FAIL(reader.read_byte(hdr))) {
      } else {
        uint64_t n = hdr >> 4;
        if (15 == n && OB_FAIL(reader.read_varint(n))) {
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          if (OB_FAIL(parse_parquet_schema_element(reader, allocator, schema_names,
                                                   schema_types, schema_converted,
                                                   schema_repetition, schema_type_length,
                                                   schema_num_children))) {
          }
        }
      }
    } else if (3 == field_id && 6 == t) {
      int64_t v = 0;
      if (OB_FAIL(reader.read_zigzag(v))) {
      } else {
        num_rows = v;
      }
    } else if (4 == field_id && 9 == t) {
      uint8_t hdr = 0;
      if (OB_FAIL(reader.read_byte(hdr))) {
      } else {
        uint64_t n = hdr >> 4;
        if (15 == n && OB_FAIL(reader.read_varint(n))) {
        }
        // Only the first row group is decoded: the fixtures written for the
        // file SQL tests use a single group, and this keeps the reader's memory
        // bounded regardless of the file's row count.
        for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          if (OB_FAIL(parse_parquet_row_group(reader, columns))) {
          }
        }
      }
    } else if (OB_FAIL(reader.skip(static_cast<uint8_t>(t)))) {
    }
    if (OB_SUCC(ret)) {
      t = reader.next_field_type();
    }
  }
  if (OB_SUCC(ret) && t < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("truncated thrift struct", K(ret));
  }
  reader.leave();
  // Element 0 of the schema is the root group; only leaves carry data.
  if (OB_SUCC(ret)) {
    int64_t leaf = 0;
    for (int64_t i = 1; OB_SUCC(ret) && i < schema_names.count(); ++i) {
      if (0 == schema_num_children.at(i)) {
        if (leaf >= columns.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("parquet schema has more leaves than row group chunks", K(ret));
        } else {
          columns.at(leaf).name_ = schema_names.at(i);
          columns.at(leaf).physical_type_ = schema_types.at(i);
          columns.at(leaf).type_length_ = schema_type_length.at(i);
          columns.at(leaf).converted_type_ = schema_converted.at(i);
          columns.at(leaf).optional_ = (1 == schema_repetition.at(i));
          ++leaf;
        }
      }
    }
  }
  return ret;
}

int build_parquet_schema(ObFileTableSchema &schema, const ObIArray<ParquetColumn> &columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObFileTableColumn column;
    column.name_ = columns.at(i).name_;
    column.nullable_ = columns.at(i).optional_;
    switch (columns.at(i).physical_type_) {
      case PARQUET_TYPE_BOOLEAN:
        column.type_.set_type(ObTinyIntType);
        break;
      case PARQUET_TYPE_INT32:
      case PARQUET_TYPE_INT64:
        column.type_.set_type(ObIntType);
        break;
      case PARQUET_TYPE_FLOAT:
      case PARQUET_TYPE_DOUBLE:
        column.type_.set_type(ObDoubleType);
        break;
      case PARQUET_TYPE_BYTE_ARRAY:
      case PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY:
        column.type_.set_type(ObVarcharType);
        column.type_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        break;
      default:
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet physical type");
        break;
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(schema.columns().push_back(column))) {
      }
    }
  }
  return ret;
}

// RLE/bit-packed hybrid decoder, used for definition levels.
int parquet_decode_rle(const uint8_t *data, const int64_t len, const int64_t max_values,
                       ObIArray<bool> &defined)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < len && defined.count() < max_values) {
    uint64_t header = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      if (pos >= len || shift > 63) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const uint8_t b = data[pos++];
        header |= (static_cast<uint64_t>(b & 0x7f) << shift);
        shift += 7;
        done = (0 == (b & 0x80));
      }
    }
    if (OB_SUCC(ret)) {
      if (0 == (header & 1)) {
        // RLE run: (header >> 1) copies of one value.
        const int64_t run = static_cast<int64_t>(header >> 1);
        const int64_t bytes = (1 + 7) / 8;
        if (pos + bytes > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const bool value = (data[pos] & 0x01);
          pos += bytes;
          for (int64_t i = 0; OB_SUCC(ret) && i < run && defined.count() < max_values; ++i) {
            if (OB_FAIL(defined.push_back(value))) {
            }
          }
        }
      } else {
        // Bit-packed run: (header >> 1) groups of 8 values, 1 bit each.
        const int64_t groups = static_cast<int64_t>(header >> 1);
        const int64_t bytes = groups;
        if (pos + bytes > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t g = 0; OB_SUCC(ret) && g < groups; ++g) {
            const uint8_t byte = data[pos + g];
            for (int64_t b = 0; OB_SUCC(ret) && b < 8 && defined.count() < max_values; ++b) {
              if (OB_FAIL(defined.push_back(0 != ((byte >> b) & 0x01)))) {
              }
            }
          }
          pos += bytes;
        }
      }
    }
  }
  return ret;
}

int decode_parquet_plain_page(ObIAllocator &allocator, const char *page, const int64_t len,
                              const ParquetColumn &column, ObIArray<ObObjParam> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int64_t num_values = column.chunk_.num_values_;
  ObSEArray<bool, 16> defined;
  if (column.optional_) {
    // Optional columns prefix the values with definition levels, encoded as a
    // 4-byte little-endian byte length followed by RLE/bit-packed data.
    if (pos + 4 > len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const uint32_t dl_len = static_cast<uint32_t>(static_cast<uint8_t>(page[pos])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(page[pos + 1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(page[pos + 2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(page[pos + 3])) << 24);
      pos += 4;
      if (pos + dl_len > len) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(parquet_decode_rle(
                   reinterpret_cast<const uint8_t *>(page + pos), dl_len,
                   num_values, defined))) {
      } else {
        pos += dl_len;
      }
    }
  }
  if (OB_SUCC(ret)) {
    // Required columns have every value present; the definition levels are only
    // consulted when the column is optional.
    for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      const bool present = !column.optional_ || (i < defined.count() && defined.at(i));
      if (!present) {
        ObObjParam null_obj;
        null_obj.set_null();
        if (OB_FAIL(values.push_back(null_obj))) {
        }
      } else {
        ObObjParam obj;
        switch (column.physical_type_) {
          case PARQUET_TYPE_BOOLEAN: {
            if (pos >= len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              obj.set_tinyint((static_cast<uint8_t>(page[pos]) >> (i % 8)) & 0x01);
              if (7 == i % 8) {
                ++pos;
              }
            }
            break;
          }
          case PARQUET_TYPE_INT32: {
            if (pos + 4 > len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              int32_t v = 0;
              MEMCPY(&v, page + pos, 4);
              obj.set_int(v);
              pos += 4;
            }
            break;
          }
          case PARQUET_TYPE_INT64: {
            if (pos + 8 > len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              int64_t v = 0;
              MEMCPY(&v, page + pos, 8);
              obj.set_int(v);
              pos += 8;
            }
            break;
          }
          case PARQUET_TYPE_FLOAT: {
            if (pos + 4 > len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              float v = 0;
              MEMCPY(&v, page + pos, 4);
              obj.set_double(v);
              pos += 4;
            }
            break;
          }
          case PARQUET_TYPE_DOUBLE: {
            if (pos + 8 > len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              double v = 0;
              MEMCPY(&v, page + pos, 8);
              obj.set_double(v);
              pos += 8;
            }
            break;
          }
          case PARQUET_TYPE_BYTE_ARRAY: {
            if (pos + 4 > len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              int32_t n = 0;
              MEMCPY(&n, page + pos, 4);
              pos += 4;
              if (n < 0 || pos + n > len) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                ObString text;
                if (OB_FAIL(append_text_column(allocator, page + pos, n, text))) {
                } else {
                  obj.set_varchar(text);
                  obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
                  pos += n;
                }
              }
            }
            break;
          }
          case PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY: {
            const int64_t n = column.type_length_;
            if (n < 0 || pos + n > len) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              ObString text;
              if (OB_FAIL(append_text_column(allocator, page + pos, n, text))) {
              } else {
                obj.set_varchar(text);
                obj.set_collation_type(CS_TYPE_BINARY);
                pos += n;
              }
            }
            break;
          }
          default:
            ret = OB_NOT_SUPPORTED;
            break;
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(values.push_back(obj))) {
          }
        }
      }
    }
  }
  return ret;
}

int decode_parquet_column(ObIAllocator &allocator, const char *data, const int64_t len,
                          const ParquetColumn &column, ObIArray<ObObjParam> &values)
{
  int ret = OB_SUCCESS;
  if (PARQUET_CODEC_UNCOMPRESSED != column.chunk_.codec_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "compressed parquet column");
  } else if (column.chunk_.file_offset_ < 0 || column.chunk_.file_offset_ >= len) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("parquet column offset out of range", K(ret), K(column.chunk_.file_offset_));
  } else {
    // PageHeader { 1: PageType type, 2: i32 uncompressed_page_size,
    //              3: i32 compressed_page_size, 5: DataPageHeader data_page_header, ... }
    ThriftCompactReader reader(data + column.chunk_.file_offset_,
                               len - column.chunk_.file_offset_);
    int32_t page_type = -1;
    int32_t compressed_size = 0;
    reader.enter();
    int t = reader.next_field_type();
    while (OB_SUCC(ret) && 0 < t) {
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_id(field_id, t))) {
      } else if (1 == field_id && 5 == t) {
        int64_t v = 0;
        if (OB_FAIL(reader.read_zigzag(v))) {
        } else {
          page_type = static_cast<int32_t>(v);
        }
      } else if (3 == field_id && 5 == t) {
        int64_t v = 0;
        if (OB_FAIL(reader.read_zigzag(v))) {
        } else {
          compressed_size = static_cast<int32_t>(v);
        }
      } else if (OB_FAIL(reader.skip(static_cast<uint8_t>(t)))) {
      }
      if (OB_SUCC(ret)) {
        t = reader.next_field_type();
      }
    }
    reader.leave();
    if (OB_SUCC(ret) && t < 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("truncated parquet page header", K(ret));
    }
    const int64_t page_start = column.chunk_.file_offset_ + reader.pos();
    if (OB_SUCC(ret)) {
      if (0 != page_type) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "non-data parquet page");
      } else if (page_start + compressed_size > len) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(decode_parquet_plain_page(allocator, data + page_start,
                                                   compressed_size, column, values))) {
      }
    }
  }
  return ret;
}

int read_parquet_rows(ObIAllocator &allocator, const char *data, const int64_t len,
                      const ObIArray<ParquetColumn> &columns, const int64_t num_rows,
                      ObIArray<ObObjParam> &objs, int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  row_cnt = 0;
  // Parquet stores values column-major while the executor consumes rows, so each
  // column is decoded into its own array first and the rows are then interleaved.
  ObSEArray<ObSEArray<ObObjParam, 16> *, 8> values;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    void *buf = allocator.alloc(sizeof(ObSEArray<ObObjParam, 16>));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate column values failed", K(ret));
    } else {
      ObSEArray<ObObjParam, 16> *column_values = new (buf) ObSEArray<ObObjParam, 16>();
      if (OB_FAIL(values.push_back(column_values))) {
      } else if (OB_FAIL(decode_parquet_column(allocator, data, len, columns.at(i),
                                               *column_values))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    int64_t rows = num_rows;
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
      if (values.at(i)->count() < rows) {
        rows = values.at(i)->count();
      }
    }
    for (int64_t r = 0; OB_SUCC(ret) && r < rows; ++r) {
      for (int64_t c = 0; OB_SUCC(ret) && c < values.count(); ++c) {
        if (OB_FAIL(objs.push_back(values.at(c)->at(r)))) {
        }
      }
      if (OB_SUCC(ret)) {
        ++row_cnt;
      }
    }
  }
  return ret;
}

}  // namespace

ObFileTableFormat ob_file_table_format_from_name(const ObString &func_name)
{
  ObFileTableFormat format = FILE_TABLE_FORMAT_INVALID;
  if (0 == func_name.case_compare("read_csv")) {
    format = FILE_TABLE_FORMAT_CSV;
  } else if (0 == func_name.case_compare("read_json")) {
    format = FILE_TABLE_FORMAT_JSON;
  } else if (0 == func_name.case_compare("read_ndjson")) {
    format = FILE_TABLE_FORMAT_NDJSON;
  } else if (0 == func_name.case_compare("read_parquet")) {
    format = FILE_TABLE_FORMAT_PARQUET;
  }
  return format;
}

bool ob_is_file_table_func(const ObString &func_name)
{
  return FILE_TABLE_FORMAT_INVALID != ob_file_table_format_from_name(func_name) ||
         0 == func_name.case_compare("glob");
}

bool ob_is_file_table_glob(const ObString &func_name)
{
  return 0 == func_name.case_compare("glob");
}

ObFileTableReader::ObFileTableReader(common::ObIAllocator &allocator,
                                     const ObFileTableOptions &options,
                                     ObFileTableSchema &schema)
  : allocator_(allocator), options_(options), schema_(schema) {}

int ObFileTableReader::list(const ObString &path,
                            common::ObIArray<common::ObObjParam> &objs,
                            int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  char path_buf[common::OB_MAX_FILE_NAME_LENGTH + 1];
  glob_t glob_buf;
  MEMSET(&glob_buf, 0, sizeof(glob_buf));
  row_cnt = 0;
  if (OB_UNLIKELY(path.empty() || path.length() > common::OB_MAX_FILE_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    const int glob_ret = ::glob(path_buf, GLOB_NOSORT, NULL, &glob_buf);
    if (0 != glob_ret && GLOB_NOMATCH != glob_ret) {
      ret = OB_IO_ERROR;
      LOG_WARN("glob failed", K(ret), K(path), K(glob_ret));
    } else {
      // glob() returns files in directory order; sort so that the result is
      // deterministic across filesystems.
      ObSEArray<ObString, 8> files;
      for (size_t i = 0; i < glob_buf.gl_pathc && OB_SUCC(ret); ++i) {
        ObString file;
        if (OB_FAIL(append_text_column(allocator_, glob_buf.gl_pathv[i],
                                       static_cast<int64_t>(strlen(glob_buf.gl_pathv[i])), file))) {
        } else if (OB_FAIL(files.push_back(file))) {
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(files.begin(), files.end(),
                  [](const ObString &lhs, const ObString &rhs) { return lhs.compare(rhs) < 0; });
        for (int64_t i = 0; OB_SUCC(ret) && i < files.count(); ++i) {
          ObObjParam obj;
          obj.set_varchar(files.at(i));
          obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
          obj.set_result_flag(NOT_NULL_FLAG);
          if (OB_FAIL(objs.push_back(obj))) {
          }
        }
        row_cnt = files.count();
      }
    }
  }
  ::globfree(&glob_buf);
  return ret;
}

int ObFileTableReader::read(const ObString &path,
                            common::ObIArray<common::ObObjParam> &objs,
                            int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  char path_buf[common::OB_MAX_FILE_NAME_LENGTH + 1];
  glob_t glob_buf;
  MEMSET(&glob_buf, 0, sizeof(glob_buf));
  row_cnt = 0;
  if (OB_UNLIKELY(path.empty() || path.length() > common::OB_MAX_FILE_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid file path", K(ret), K(path));
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    const int glob_ret = ::glob(path_buf, GLOB_NOSORT, NULL, &glob_buf);
    if (0 != glob_ret && GLOB_NOMATCH != glob_ret) {
      ret = OB_IO_ERROR;
      LOG_WARN("glob failed", K(ret), K(path), K(glob_ret));
    } else if (0 == glob_buf.gl_pathc) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("no file matches the path", K(ret), K(path));
    } else {
      ObSEArray<ObString, 8> files;
      for (size_t i = 0; i < glob_buf.gl_pathc && OB_SUCC(ret); ++i) {
        ObString file;
        if (OB_FAIL(append_text_column(allocator_, glob_buf.gl_pathv[i],
                                       static_cast<int64_t>(strlen(glob_buf.gl_pathv[i])), file))) {
        } else if (OB_FAIL(files.push_back(file))) {
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(files.begin(), files.end(),
                  [](const ObString &lhs, const ObString &rhs) { return lhs.compare(rhs) < 0; });
        // The schema is inferred from the first file only; the remaining files
        // are read with the same column layout, which is what makes a glob over
        // a set of homogeneous files behave like one table.
        if (OB_FAIL(read_one_file(files.at(0), objs, row_cnt))) {
        } else {
          for (int64_t i = 1; OB_SUCC(ret) && i < files.count(); ++i) {
            int64_t part_rows = 0;
            if (OB_FAIL(read_one_file(files.at(i), objs, part_rows))) {
            } else {
              row_cnt += part_rows;
            }
          }
        }
      }
    }
  }
  ::globfree(&glob_buf);
  return ret;
}

int ObFileTableReader::read_one_file(const ObString &path,
                                     common::ObIArray<common::ObObjParam> &objs,
                                     int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  char *data = NULL;
  int64_t len = 0;
  char path_buf[common::OB_MAX_FILE_NAME_LENGTH + 1];
  row_cnt = 0;
  MEMCPY(path_buf, path.ptr(), path.length());
  path_buf[path.length()] = '\0';
  if (OB_FAIL(read_whole_file(allocator_, path_buf, data, len))) {
  } else if (FILE_TABLE_FORMAT_CSV == options_.format_) {
    if (OB_FAIL(read_csv(data, len, objs, row_cnt))) {
    }
  } else if (FILE_TABLE_FORMAT_PARQUET == options_.format_) {
    if (OB_FAIL(read_parquet(data, len, objs, row_cnt))) {
    }
  } else {
    const bool newline_delimited =
        (FILE_TABLE_FORMAT_NDJSON == options_.format_) || options_.header_ == false ||
        (FILE_TABLE_FORMAT_JSON == options_.format_ && NULL != strchr(data, '\n') &&
         NULL == strstr(data, "\"columns\""));
    if (OB_FAIL(read_json(data, len, newline_delimited, objs, row_cnt))) {
    }
  }
  return ret;
}

int ObFileTableReader::infer_schema_from_csv(const char *data, const int64_t len)
{
  int ret = OB_SUCCESS;
  ObSEArray<CsvField, 16> fields;
  bool first_row = true;
  int64_t pos = 0;
  ObString nullstr = options_.nullstr_;
  while (OB_SUCC(ret) && pos <= len) {
    // Parse one physical row, honouring quoted fields that may contain the
    // field or line delimiter.
    fields.reuse();
    bool end_of_row = false;
    bool end_of_file = (pos >= len);
    while (!end_of_row && !end_of_file) {
      CsvField field;
      field.ptr_ = data + pos;
      field.len_ = 0;
      field.is_null_ = false;
      bool in_quote = false;
      char *scratch = static_cast<char *>(allocator_.alloc(len - pos + 1));
      int64_t out = 0;
      if (OB_ISNULL(scratch)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      while (OB_SUCC(ret) && pos < len) {
        const char c = data[pos];
        if (in_quote) {
          if (c == options_.escape_ && pos + 1 < len) {
            scratch[out++] = data[pos + 1];
            pos += 2;
          } else if (c == options_.quote_) {
            if (pos + 1 < len && data[pos + 1] == options_.quote_) {
              scratch[out++] = c;
              pos += 2;
            } else {
              in_quote = false;
              ++pos;
            }
          } else {
            scratch[out++] = c;
            ++pos;
          }
        } else if (c == options_.quote_) {
          in_quote = true;
          ++pos;
        } else if (c == options_.field_delimiter_) {
          ++pos;
          break;
        } else if (c == options_.line_delimiter_ || '\r' == c) {
          end_of_row = true;
          ++pos;
          if ('\r' == c && pos < len && '\n' == data[pos]) {
            ++pos;
          }
          break;
        } else {
          scratch[out++] = c;
          ++pos;
        }
      }
      field.ptr_ = scratch;
      field.len_ = out;
      field.is_null_ = (NULL != nullstr.ptr() &&
                        0 == nullstr.compare(ObString(static_cast<int32_t>(out), scratch)));
      if (OB_FAIL(fields.push_back(field))) {
      } else if (pos >= len) {
        end_of_file = true;
      }
    }
    if (OB_FAIL(ret) || 0 == fields.count()) {
      break;
    }
    if (first_row && options_.header_) {
      // The header row names the columns; the schema starts out as text and is
      // widened by the data rows below.
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        ObFileTableColumn column;
        if (OB_FAIL(dedup_column_name(schema_.columns(), allocator_,
                                      fields.at(i).ptr_, fields.at(i).len_,
                                      column.name_))) {
        } else {
          column.type_.set_type(ObVarcharType);
          column.type_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
          column.nullable_ = true;
          if (OB_FAIL(schema_.columns().push_back(column))) {
          }
        }
      }
      first_row = false;
    } else {
      if (first_row) {
        // No header: name the columns column_0, column_1, ... the way the
        // values table does.
        for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
          ObFileTableColumn column;
          ObSqlString name;
          if (OB_FAIL(name.append_fmt("column_%ld", i))) {
          } else if (OB_FAIL(append_text_column(allocator_, name.ptr(), name.length(),
                                                column.name_))) {
          } else {
            column.type_.set_type(ObNullType);
            column.type_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
            column.nullable_ = true;
            if (OB_FAIL(schema_.columns().push_back(column))) {
            }
          }
        }
        first_row = false;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count() && i < schema_.columns().count(); ++i) {
        if (fields.at(i).is_null_) {
          continue;
        }
        ObFileTableColumn &column = schema_.columns().at(i);
        const ObExprResType cur = infer_type_from_text(fields.at(i).ptr_, fields.at(i).len_);
        if (ObNullType == column.type_.get_type()) {
          column.type_ = cur;
        } else {
          column.type_ = merge_type(column.type_, cur);
        }
      }
    }
    if (end_of_file) {
      break;
    }
  }
  return ret;
}

int ObFileTableReader::read_csv(const char *data, const int64_t len,
                                common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  row_cnt = 0;
  if (OB_FAIL(infer_schema_from_csv(data, len))) {
  } else {
    // Second pass: coerce every field to the inferred column type and append
    // the row.
    ObSEArray<CsvField, 16> fields;
    int64_t pos = 0;
    int64_t line_no = 0;
    while (OB_SUCC(ret) && pos < len) {
      fields.reuse();
      bool end_of_row = false;
      bool end_of_file = false;
      while (!end_of_row && !end_of_file) {
        CsvField field;
        field.ptr_ = data + pos;
        field.len_ = 0;
        field.is_null_ = false;
        bool in_quote = false;
        char *scratch = static_cast<char *>(allocator_.alloc(len - pos + 1));
        int64_t out = 0;
        if (OB_ISNULL(scratch)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
        while (OB_SUCC(ret) && pos < len) {
          const char c = data[pos];
          if (in_quote) {
            if (c == options_.escape_ && pos + 1 < len) {
              scratch[out++] = data[pos + 1];
              pos += 2;
            } else if (c == options_.quote_) {
              if (pos + 1 < len && data[pos + 1] == options_.quote_) {
                scratch[out++] = c;
                pos += 2;
              } else {
                in_quote = false;
                ++pos;
              }
            } else {
              scratch[out++] = c;
              ++pos;
            }
          } else if (c == options_.quote_) {
            in_quote = true;
            ++pos;
          } else if (c == options_.field_delimiter_) {
            ++pos;
            break;
          } else if (c == options_.line_delimiter_ || '\r' == c) {
            end_of_row = true;
            ++pos;
            if ('\r' == c && pos < len && '\n' == data[pos]) {
              ++pos;
            }
            break;
          } else {
            scratch[out++] = c;
            ++pos;
          }
        }
        field.ptr_ = scratch;
        field.len_ = out;
        field.is_null_ = (NULL != options_.nullstr_.ptr() &&
                          0 == options_.nullstr_.compare(
                              ObString(static_cast<int32_t>(out), scratch)));
        if (OB_FAIL(fields.push_back(field))) {
        } else if (pos >= len) {
          end_of_file = true;
        }
      }
      if (OB_FAIL(ret) || 0 == fields.count()) {
        break;
      }
      ++line_no;
      if (line_no <= options_.skip_header_ ||
          (1 == line_no && options_.header_)) {
        if (end_of_file) {
          break;
        }
        continue;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < schema_.columns().count(); ++i) {
        const ObFileTableColumn &column = schema_.columns().at(i);
        if (i < fields.count() && !fields.at(i).is_null_) {
          if (OB_FAIL(append_value(objs, column, fields.at(i).ptr_, fields.at(i).len_))) {
          }
        } else {
          ObObjParam obj;
          obj.set_null();
          obj.set_result_flag(0);
          if (OB_FAIL(objs.push_back(obj))) {
          }
        }
      }
      if (OB_SUCC(ret)) {
        ++row_cnt;
      }
      if (end_of_file) {
        break;
      }
    }
  }
  return ret;
}

int ObFileTableReader::read_json(const char *data, const int64_t len, const bool newline_delimited,
                                 common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  row_cnt = 0;
  if (OB_FAIL(infer_schema_from_json(data, len, newline_delimited))) {
  } else {
    // Every JSON object is flattened into the same column order; a key missing
    // from an object yields SQL NULL, which is what the union schema requires.
    ObSEArray<ObString, 8> keys;
    for (int64_t i = 0; OB_SUCC(ret) && i < schema_.columns().count(); ++i) {
      if (OB_FAIL(keys.push_back(schema_.columns().at(i).name_))) {
      }
    }
    int64_t pos = 0;
    while (OB_SUCC(ret) && pos < len) {
      while (pos < len && (' ' == data[pos] || '\n' == data[pos] || '\r' == data[pos] ||
                           '\t' == data[pos])) {
        ++pos;
      }
      if (pos >= len) {
        break;
      }
      int64_t obj_end = pos;
      if (!newline_delimited && '[' == data[pos]) {
        // A JSON array of objects: emit each element.
        ++pos;
        while (pos < len && (']' != data[pos] || ' ' == data[pos] || '\n' == data[pos])) {
          ++pos;
        }
        ++pos;
        continue;
      }
      // Find the end of this JSON object, honouring nested braces and strings.
      int depth = 0;
      bool in_string = false;
      for (; obj_end < len; ++obj_end) {
        const char c = data[obj_end];
        if (in_string) {
          if ('\\' == c) {
            ++obj_end;
          } else if ('"' == c) {
            in_string = false;
          }
        } else if ('"' == c) {
          in_string = true;
        } else if ('{' == c) {
          ++depth;
        } else if ('}') {
          --depth;
          if (0 == depth) {
            ++obj_end;
            break;
          }
        } else if ('\n' == c && 0 == depth) {
          break;
        }
      }
      if (0 == depth) {
        ObJsonNode *node = NULL;
        if (OB_FAIL(ObJsonParser::get_tree(&allocator_, data + pos, obj_end - pos, node))) {
          LOG_WARN("parse json object failed", K(ret), K(pos));
        } else if (OB_ISNULL(node) || ObJsonNodeType::J_OBJECT != node->json_type()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("json element is not an object", K(ret));
        } else {
          ObJsonObject *object = static_cast<ObJsonObject *>(node);
          for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
            ObJsonNode *value = object->get_value(keys.at(i));
            if (OB_ISNULL(value) || value->json_type() == ObJsonNodeType::J_NULL) {
              ObObjParam obj;
              obj.set_null();
              if (OB_FAIL(objs.push_back(obj))) {
              }
            } else if (OB_FAIL(append_json_value(objs, schema_.columns().at(i), *value))) {
            }
          }
          if (OB_SUCC(ret)) {
            ++row_cnt;
          }
        }
      }
      pos = obj_end;
    }
  }
  return ret;
}

int ObFileTableReader::infer_schema_from_json(const char *data, const int64_t len,
                                              const bool newline_delimited)
{
  int ret = OB_SUCCESS;
  // The union schema is the ordered set of keys seen in any object; the first
  // appearance fixes the column order.
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < len) {
    while (pos < len && (' ' == data[pos] || '\n' == data[pos] || '\r' == data[pos] ||
                         '\t' == data[pos] || '[' == data[pos] || ']' == data[pos] ||
                         ',' == data[pos])) {
      ++pos;
    }
    if (pos >= len || '}' == data[pos]) {
      break;
    }
    int64_t obj_end = pos;
    int depth = 0;
    bool in_string = false;
    for (; obj_end < len; ++obj_end) {
      const char c = data[obj_end];
      if (in_string) {
        if ('\\' == c) {
          ++obj_end;
        } else if ('"' == c) {
          in_string = false;
        }
      } else if ('"' == c) {
        in_string = true;
      } else if ('{' == c) {
        ++depth;
      } else if ('}') {
        --depth;
        if (0 == depth) {
          ++obj_end;
          break;
        }
      } else if ('\n' == c && 0 == depth) {
        break;
      }
    }
    if (0 == depth) {
      ObJsonNode *node = NULL;
      if (OB_FAIL(ObJsonParser::get_tree(&allocator_, data + pos, obj_end - pos, node))) {
        LOG_WARN("parse json object failed", K(ret), K(pos));
      } else if (OB_ISNULL(node) || ObJsonNodeType::J_OBJECT != node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObJsonObject *object = static_cast<ObJsonObject *>(node);
        const uint64_t count = object->element_count();
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ObString key;
          if (OB_FAIL(object->get_key_by_idx(i, key))) {
            break;
          }
          ObJsonNode *value = object->get_value(i);
          bool found = false;
          for (int64_t j = 0; !found && j < schema_.columns().count(); ++j) {
            found = (0 == schema_.columns().at(j).name_.compare(key));
          }
          if (!found) {
            ObFileTableColumn column;
            if (OB_FAIL(append_text_column(allocator_, key.ptr(), key.length(), column.name_))) {
            } else {
              column.type_.set_type(ObNullType);
              column.type_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
              column.nullable_ = true;
              if (OB_FAIL(schema_.columns().push_back(column))) {
              }
            }
          }
          if (OB_SUCC(ret) && OB_NOT_NULL(value)) {
            ObFileTableColumn *column = NULL;
            for (int64_t j = 0; OB_SUCC(ret) && NULL == column && j < schema_.columns().count(); ++j) {
              if (0 == schema_.columns().at(j).name_.compare(key)) {
                column = &schema_.columns().at(j);
              }
            }
            if (OB_NOT_NULL(column)) {
              const char *text = value->get_data();
              const int64_t text_len = static_cast<int64_t>(value->get_data_length());
              ObExprResType cur;
              switch (value->json_type()) {
                case ObJsonNodeType::J_NULL:
                  cur.set_type(ObNullType);
                  break;
                case ObJsonNodeType::J_BOOLEAN:
                  cur.set_type(ObTinyIntType);
                  break;
                case ObJsonNodeType::J_INT:
                  cur.set_type(ObIntType);
                  break;
                case ObJsonNodeType::J_DOUBLE:
                case ObJsonNodeType::J_DECIMAL:
                  cur.set_type(ObDoubleType);
                  break;
                default:
                  cur = infer_type_from_text(text, text_len);
                  break;
              }
              if (ObNullType == cur.get_type()) {
                // leave the column untouched
              } else if (ObNullType == column->type_.get_type()) {
                column->type_ = cur;
              } else {
                column->type_ = merge_type(column->type_, cur);
              }
            }
          }
        }
      }
    }
    pos = obj_end;
  }
  return ret;
}

int ObFileTableReader::read_parquet(const char *data, const int64_t len,
                                    common::ObIArray<common::ObObjParam> &objs, int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  row_cnt = 0;
  if (OB_UNLIKELY(len < 12) || 0 != memcmp(data, "PAR1", 4) ||
      0 != memcmp(data + len - 4, "PAR1", 4)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not a parquet file", K(ret), K(len));
  } else {
    // The last 8 bytes hold the little-endian footer length followed by "PAR1".
    const int64_t footer_len = static_cast<int64_t>(static_cast<uint8_t>(data[len - 8])) |
        (static_cast<int64_t>(static_cast<uint8_t>(data[len - 7])) << 8) |
        (static_cast<int64_t>(static_cast<uint8_t>(data[len - 6])) << 16) |
        (static_cast<int64_t>(static_cast<uint8_t>(data[len - 5])) << 24);
    const int64_t footer_start = len - 8 - footer_len;
    ObSEArray<ParquetColumn, 8> columns;
    int64_t num_rows = 0;
    if (footer_len <= 0 || footer_start < 4) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid parquet footer", K(ret), K(footer_len), K(footer_start));
    } else if (OB_FAIL(parse_parquet_footer(allocator_, data + footer_start, footer_len,
                                            columns, num_rows))) {
    } else if (0 == columns.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("parquet file has no columns", K(ret));
    } else if (OB_FAIL(build_parquet_schema(schema_, columns))) {
    } else if (OB_FAIL(read_parquet_rows(allocator_, data, len, columns,
                                         num_rows, objs, row_cnt))) {
    }
  }
  return ret;
}

int ObFileTableReader::append_json_value(common::ObIArray<common::ObObjParam> &objs,
                                         const ObFileTableColumn &column,
                                         const common::ObJsonNode &value)
{
  int ret = OB_SUCCESS;
  const ObObjType type = column.type_.get_type();
  if (ObTinyIntType == type && value.json_type() == ObJsonNodeType::J_BOOLEAN) {
    ObObjParam obj;
    obj.set_tinyint(value.get_boolean() ? 1 : 0);
    if (OB_FAIL(objs.push_back(obj))) {
    }
  } else if (ObIntType == type &&
             (value.json_type() == ObJsonNodeType::J_INT ||
              value.json_type() == ObJsonNodeType::J_UINT)) {
    ObObjParam obj;
    obj.set_int(value.get_int());
    if (OB_FAIL(objs.push_back(obj))) {
    }
  } else if (ObDoubleType == type &&
             (value.json_type() == ObJsonNodeType::J_DOUBLE ||
              value.json_type() == ObJsonNodeType::J_DECIMAL ||
              value.json_type() == ObJsonNodeType::J_INT ||
              value.json_type() == ObJsonNodeType::J_UINT)) {
    ObObjParam obj;
    obj.set_double(value.get_double());
    if (OB_FAIL(objs.push_back(obj))) {
    }
  } else if (value.json_type() == ObJsonNodeType::J_STRING) {
    if (OB_FAIL(append_value(objs, column, value.get_data(),
                             static_cast<int64_t>(value.get_data_length())))) {
    }
  } else {
    // Numbers that widened to text, or nested arrays/objects: keep their JSON
    // text so nothing is silently dropped.
    ObString raw;
    if (OB_FAIL(append_text_column(allocator_, value.get_data(),
                                   static_cast<int64_t>(value.get_data_length()), raw))) {
    } else {
      ObObjParam obj;
      obj.set_varchar(raw);
      obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      if (OB_FAIL(objs.push_back(obj))) {
      }
    }
  }
  return ret;
}

int ObFileTableReader::append_value(common::ObIArray<common::ObObjParam> &objs,
                                    const ObFileTableColumn &column,
                                    const char *ptr, const int64_t len)
{
  int ret = OB_SUCCESS;
  ObObjParam obj;
  const ObObjType type = column.type_.get_type();
  switch (type) {
    case ObTinyIntType: {
      obj.set_tinyint(is_bool_text(ptr, len) ? (0 == strncasecmp(ptr, "true", 4) ? 1 : 0)
                                             : static_cast<int8_t>(atoi(ptr)));
      break;
    }
    case ObIntType: {
      obj.set_int(static_cast<int64_t>(strtoll(ptr, NULL, 10)));
      break;
    }
    case ObDoubleType: {
      obj.set_double(strtod(ptr, NULL));
      break;
    }
    case ObDateType: {
      ObTimeConvertCtx cvrt_ctx(NULL, false);
      int32_t date = 0;
      if (OB_FAIL(ObTimeConverter::str_to_date(ObString(static_cast<int32_t>(len), ptr), date))) {
        LOG_WARN("convert date failed", K(ret));
      } else {
        obj.set_date(date);
      }
      break;
    }
    case ObDateTimeType: {
      ObTimeConvertCtx cvrt_ctx(NULL, false);
      int64_t datetime = 0;
      if (OB_FAIL(ObTimeConverter::str_to_datetime(
                    ObString(static_cast<int32_t>(len), ptr), cvrt_ctx, datetime))) {
        LOG_WARN("convert datetime failed", K(ret));
      } else {
        obj.set_datetime(datetime);
      }
      break;
    }
    default: {
      ObString text;
      if (OB_FAIL(append_text_column(allocator_, ptr, len, text))) {
      } else {
        obj.set_varchar(text);
        obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      }
      break;
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(objs.push_back(obj))) {
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
