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

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <zvec/ailego/encoding/json/mod_json_plus.h>
#include "tokenizer.h"

namespace zvec::fts {

/*! Token Filter abstract interface
 *  Post-process tokenization results, such as case conversion, stopword
 * filtering, etc.
 */
class TokenFilter {
 public:
  virtual ~TokenFilter() = default;

  /*! Initialise the filter from a JSON configuration object.
   *  Must be called once before filter().
   *  \param config  JSON object containing filter-specific parameters.
   *  \return        true on success, false on failure.
   */
  virtual bool init(const ailego::JsonObject & /*config*/) {
    return true;
  }

  /*! Filter/transform a list of tokens.
   *  \param tokens  input token list (may be modified in place)
   *  \return        processed token list
   */
  virtual std::vector<Token> filter(std::vector<Token> tokens) const = 0;

  virtual Status validate_streaming() const {
    return Status::NotSupported("Unbounded FTS filter: ", name());
  }
  virtual Status filter_one(std::vector<Token> *tokens, size_t) const {
    *tokens = filter(std::move(*tokens));
    return Status::OK();
  }

  /*! Return filter name
   */
  virtual const char *name() const = 0;
};

using TokenFilterPtr = std::shared_ptr<TokenFilter>;

/*! Lowercase Token Filter
 *  Convert all token text to lowercase (supports full Unicode via utf8proc)
 */
class LowercaseTokenFilter : public TokenFilter {
 public:
  // Deliver an already lowercase token directly from the tokenizer. Reuse a
  // bounded string across tokens only when a code point actually changes.
  Status analyze_one(std::string_view text, uint32_t offset, uint32_t position,
                     size_t budget, std::string *scratch,
                     const TokenSink &sink) const;
  Status validate_streaming() const override {
    return Status::OK();
  }
  std::vector<Token> filter(std::vector<Token> tokens) const override;

  const char *name() const override {
    return "lowercase";
  }
};

}  // namespace zvec::fts
