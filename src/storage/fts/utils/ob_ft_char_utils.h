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

#ifndef _OCEANBASE_STORAGE_FTS_UTILS_CHAR_UTILS_H_
#define _OCEANBASE_STORAGE_FTS_UTILS_CHAR_UTILS_H_

#include <cstdint>

#define true_word_char(ctype, character) ((ctype) & (_MY_U | _MY_L | _MY_NMR) || (character) == '_')

namespace oceanbase
{
namespace storage
{
struct ObAsciiWordTable {
  uint8_t table[256];
  constexpr ObAsciiWordTable() : table{} {
    for (int i = 'a'; i <= 'z'; ++i) table[i] = 1;
    for (int i = 'A'; i <= 'Z'; ++i) table[i] = 1;
    for (int i = '0'; i <= '9'; ++i) table[i] = 1;
    table[static_cast<uint8_t>('_')] = 1;
  }
  constexpr bool is_word(const uint8_t c) const { return table[c] != 0; }
};
static constexpr ObAsciiWordTable G_ASCII_WORD_TABLE;
} // namespace storage
} // namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_UTILS_CHAR_UTILS_H_
