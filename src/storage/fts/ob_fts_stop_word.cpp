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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  is_stopword = false;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const common::ObString word_str = word.get_word().get_string();
    const common::ObCollationType coll_type = word.get_collation_type();
    if (common::ObCharset::charset_type_by_coll(coll_type) == common::CHARSET_UTF8MB4) {
      // Built-in stopwords in ob_stop_word_list are all 1..5 byte ASCII words.
      if (word_str.length() > 5 || static_cast<uint8_t>(word_str.ptr()[0]) >= 0x80) {
        is_stopword = false;
      } else {
        bool all_ascii = true;
        for (int32_t i = 1; i < word_str.length(); ++i) {
          if (static_cast<uint8_t>(word_str.ptr()[i]) >= 0x80) {
            all_ascii = false;
            break;
          }
        }
        if (!all_ascii) {
          is_stopword = false;
        } else {
          ObFTWord converted(word_str.length(), word_str.ptr(), stopword_type_);
          ret = stopword_set_.exist_refactored(converted);
          if (OB_HASH_NOT_EXIST == ret) {
            is_stopword = false;
            ret = OB_SUCCESS;
          } else if (OB_HASH_EXIST == ret) {
            is_stopword = true;
            ret = OB_SUCCESS;
          } else if (OB_SUCC(ret)) {
            ret = OB_ERR_UNEXPECTED;
          }
        }
      }
    } else {
      common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
      common::ObString cmp_str;
      if (OB_FAIL(common::ObCharset::charset_convert(
                                         allocator,
                                         word_str,
                                         coll_type,
                                         stopword_type_.get_collation_type(),
                                         cmp_str))) {
      } else {
        ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
        ret = stopword_set_.exist_refactored(converted);
        if (OB_HASH_NOT_EXIST == ret) {
          is_stopword = false;
          ret = OB_SUCCESS;
        } else if (OB_HASH_EXIST == ret) {
          is_stopword = true;
          ret = OB_SUCCESS;
        } else if (OB_SUCC(ret)) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    cs_(common::ObCharset::get_charset(meta.get_collation_type())),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    const ObString src_str = src.get_word().get_string();
    const char *ptr = src_str.ptr();
    const int32_t len = src_str.length();
    if (OB_ISNULL(cs_)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (common::ObCharset::charset_type_by_coll(word_meta_.get_collation_type()) == common::CHARSET_UTF8MB4
               && len <= static_cast<int32_t>(sizeof(casedown_buf_))) {
      bool all_ascii = true;
      bool has_upper_ascii = false;
      bool all_caseless_cjk = true;
      for (int32_t i = 0; i < len; ) {
        const uint8_t b0 = static_cast<uint8_t>(ptr[i]);
        if (b0 < 0x80) {
          all_caseless_cjk = false;
          if (b0 >= 'A' && b0 <= 'Z') {
            has_upper_ascii = true;
          }
          ++i;
        } else {
          all_ascii = false;
          if (i + 2 < len && b0 >= 0xE4 && b0 <= 0xE9
              && (static_cast<uint8_t>(ptr[i + 1]) & 0xC0) == 0x80
              && (static_cast<uint8_t>(ptr[i + 2]) & 0xC0) == 0x80) {
            i += 3;
          } else {
            all_caseless_cjk = false;
            break;
          }
        }
      }
      if ((all_ascii && !has_upper_ascii) || all_caseless_cjk) {
        dst = src;
      } else if (all_ascii) {
        for (int32_t i = 0; i < len; ++i) {
          const char c = ptr[i];
          casedown_buf_[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }
        dst = ObFTWord(len, casedown_buf_, word_meta_);
      } else if (len * cs_->casedn_multiply <= static_cast<int32_t>(sizeof(casedown_buf_))) {
        size_t size = 0;
        if (1 == cs_->casedn_multiply) {
          MEMCPY(casedown_buf_, ptr, len);
          size = cs_->cset->casedn(cs_, casedown_buf_, len, casedown_buf_, len);
        } else {
          size = cs_->cset->casedn(cs_, const_cast<char *>(ptr), len, casedown_buf_, sizeof(casedown_buf_));
        }
        dst = ObFTWord(static_cast<int64_t>(size), casedown_buf_, word_meta_);
      } else {
        ObString dst_str;
        if (OB_FAIL(ObCharset::tolower(cs_, src_str, dst_str, allocator_))) {
        } else {
          dst = ObFTWord(dst_str.length(), dst_str.ptr(), word_meta_);
        }
      }
    } else if (len * cs_->casedn_multiply <= static_cast<int32_t>(sizeof(casedown_buf_))) {
      size_t size = 0;
      if (1 == cs_->casedn_multiply) {
        MEMCPY(casedown_buf_, ptr, len);
        size = cs_->cset->casedn(cs_, casedown_buf_, len, casedown_buf_, len);
      } else {
        size = cs_->cset->casedn(cs_, const_cast<char *>(ptr), len, casedown_buf_, sizeof(casedown_buf_));
      }
      dst = ObFTWord(static_cast<int64_t>(size), casedown_buf_, word_meta_);
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(cs_, src_str, dst_str, allocator_))) {
      } else {
        dst = ObFTWord(dst_str.length(), dst_str.ptr(), word_meta_);
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    ObString copied_str;
    if (OB_FAIL(common::ob_write_string(allocator_, word.get_word().get_string(), copied_str))) {
    } else {
      ObFTWord persisted_word(copied_str.length(), copied_str.ptr(), word_meta_);
      if (OB_FAIL(word_map_.set_refactored(persisted_word, 1/*word count*/))) {
      }
    }
  } else {
    int64_t *cnt_ptr = word_map_.get(word);
    if (OB_NOT_NULL(cnt_ptr)) {
      *cnt_ptr += word_freq;
    } else {
      ObString copied_str;
      if (OB_FAIL(common::ob_write_string(allocator_, word.get_word().get_string(), copied_str))) {
      } else {
        ObFTWord persisted_word(copied_str.length(), copied_str.ptr(), word_meta_);
        if (OB_FAIL(word_map_.set_refactored(persisted_word, 1 == word_freq ? 1 : word_freq, 1/*overwrite*/))) {
        }
      }
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
