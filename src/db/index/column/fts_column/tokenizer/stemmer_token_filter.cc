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

#include "stemmer_token_filter.h"
#include <climits>
#include <unordered_map>
#include <zvec/ailego/logger/logger.h>
#include "snowball_workspace.h"

extern "C" {
#include <libstemmer.h>
}

namespace zvec::fts {

struct ThreadLocalStemmerCache {
  std::unordered_map<std::string, struct sb_stemmer *> stemmers;

  ~ThreadLocalStemmerCache() {
    for (auto &[_, s] : stemmers) {
      sb_stemmer_delete(s);
    }
  }

  struct sb_stemmer *get(const std::string &lang) {
    auto it = stemmers.find(lang);
    if (it != stemmers.end()) {
      return it->second;
    }
    auto *s = sb_stemmer_new(lang.c_str(), nullptr);
    if (s) {
      stemmers[lang] = s;
    }
    return s;
  }
};

bool StemmerTokenFilter::init(const ailego::JsonObject &config) {
  std::string lang;
  if (config.get("stemmer_lang", &lang) && !lang.empty()) {
    language_ = lang;
  }
  auto *test_stemmer = sb_stemmer_new(language_.c_str(), nullptr);
  if (!test_stemmer) {
    LOG_ERROR("[StemmerTokenFilter] failed to create stemmer for language: %s",
              language_.c_str());
    return false;
  }
  sb_stemmer_delete(test_stemmer);
  return true;
}

std::vector<Token> StemmerTokenFilter::filter(std::vector<Token> tokens) const {
  static thread_local ThreadLocalStemmerCache tls_cache;
  auto *stemmer = tls_cache.get(language_);
  if (!stemmer) {
    return tokens;
  }
  for (auto &token : tokens) {
    const auto *result = sb_stemmer_stem(
        stemmer, reinterpret_cast<const unsigned char *>(token.text.data()),
        static_cast<int>(token.text.size()));
    if (result) {
      int len = sb_stemmer_length(stemmer);
      token.text.assign(reinterpret_cast<const char *>(result), len);
    }
  }
  return tokens;
}

Status StemmerTokenFilter::validate_streaming() const {
  return Status::OK();
}

Status StemmerTokenFilter::filter_one(std::vector<Token> *tokens,
                                      size_t budget) const {
  auto &token = tokens->front();
  // Each 3.1.1 algorithm creates a fixed scalar environment and at most
  // two tiny initial strings. All subsequent string growth (including the
  // auxiliary string in Danish, Dutch and Finnish) uses increase_size().
  if (budget < 4096 || token.text.size() > INT_MAX) {
    return {StatusCode::RESOURCE_EXHAUSTED,
            "Cannot reserve Snowball workspace"};
  }
  size_t remaining = budget - 4096;
  struct RestoreBudget {
    size_t *previous;
    ~RestoreBudget() {
      zvec_fts_set_snowball_budget(previous);
    }
  } scope{zvec_fts_set_snowball_budget(&remaining)};
  // Scope Snowball's mutable environment to this reservation. The legacy
  // batch API can keep a thread-local cache larger than this writer's budget.
  std::unique_ptr<sb_stemmer, decltype(&sb_stemmer_delete)> stemmer(
      sb_stemmer_new(language_.c_str(), nullptr), &sb_stemmer_delete);
  if (!stemmer) {
    return {StatusCode::RESOURCE_EXHAUSTED,
            "Cannot reserve Snowball workspace"};
  }
  const auto *word = sb_stemmer_stem(
      stemmer.get(), reinterpret_cast<const unsigned char *>(token.text.data()),
      static_cast<int>(token.text.size()));
  if (!word) {
    return {StatusCode::RESOURCE_EXHAUSTED, "Snowball workspace exhausted"};
  }
  const size_t result_bytes =
      static_cast<size_t>(sb_stemmer_length(stemmer.get()));
  if (result_bytes > budget / 4) {
    return {StatusCode::RESOURCE_EXHAUSTED,
            "Snowball output exceeds filter workspace"};
  }
  token.text.assign(reinterpret_cast<const char *>(word), result_bytes);
  return Status::OK();
}

}  // namespace zvec::fts
