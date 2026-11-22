// Copyright 2025-present the zvec project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "token_filter.h"
#include <utf8proc.h>
#include <string>

namespace zvec::fts {

Status LowercaseTokenFilter::analyze_one(std::string_view text, uint32_t offset,
                                         uint32_t position, size_t budget,
                                         std::string *scratch,
                                         const TokenSink &sink) const {
  size_t copied = 0;
  scratch->clear();
  for (size_t i = 0; i < text.size();) {
    const auto first = static_cast<uint8_t>(text[i]);
    utf8proc_int32_t cp = first;
    size_t length = 1;
    if (first >= 128) {
      auto n = utf8proc_iterate(
          reinterpret_cast<const utf8proc_uint8_t *>(text.data() + i),
          text.size() - i, &cp);
      if (n <= 0) {
        ++i;
        continue;
      }
      length = static_cast<size_t>(n);
    }
    const auto lower = first < 128
                           ? (first >= 'A' && first <= 'Z' ? cp + 32 : cp)
                           : utf8proc_tolower(cp);
    if (lower != cp) {
      if (!copied) {
        // Reserve for four encoded bytes per input byte and the old
        // allocation while growing. Subsequent appends never reallocate.
        if (budget < 1024 || text.size() > (budget - 1024) / 16) {
          return {StatusCode::RESOURCE_EXHAUSTED,
                  "FTS lowercase workspace exhausted"};
        }
        scratch->reserve(4 * text.size());
      }
      scratch->append(text.data() + copied, i - copied);
      utf8proc_uint8_t bytes[4];
      auto n = utf8proc_encode_char(lower, bytes);
      scratch->append(reinterpret_cast<const char *>(bytes), n);
      copied = i + length;
    }
    i += length;
  }
  if (!copied) {
    return sink(text, offset, position);
  }
  scratch->append(text.data() + copied, text.size() - copied);
  return sink(*scratch, offset, position);
}

std::vector<Token> LowercaseTokenFilter::filter(
    std::vector<Token> tokens) const {
  for (auto &token : tokens) {
    std::string result;
    result.reserve(token.text.size());
    const auto *str =
        reinterpret_cast<const utf8proc_uint8_t *>(token.text.data());
    auto len = static_cast<utf8proc_ssize_t>(token.text.size());
    utf8proc_ssize_t pos = 0;
    while (pos < len) {
      utf8proc_int32_t codepoint;
      utf8proc_ssize_t bytes =
          utf8proc_iterate(str + pos, len - pos, &codepoint);
      if (bytes < 1) {
        result.push_back(token.text[pos]);
        ++pos;
        continue;
      }
      utf8proc_int32_t lower = utf8proc_tolower(codepoint);
      utf8proc_uint8_t buf[4];
      utf8proc_ssize_t written = utf8proc_encode_char(lower, buf);
      result.append(reinterpret_cast<const char *>(buf),
                    static_cast<size_t>(written));
      pos += bytes;
    }
    token.text = std::move(result);
  }
  return tokens;
}

}  // namespace zvec::fts
