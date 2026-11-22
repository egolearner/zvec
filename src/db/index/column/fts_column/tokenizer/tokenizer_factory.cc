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

#include "tokenizer_factory.h"
#include <zvec/ailego/encoding/json/mod_json_plus.h>
#include <zvec/ailego/logger/logger.h>
#include "ascii_folding_token_filter.h"
#include "jieba_tokenizer.h"
#include "ngram_tokenizer.h"
#include "standard_tokenizer.h"
#include "stemmer_token_filter.h"
#include "tokenizer_pipeline_manager.h"
#include "whitespace_tokenizer.h"

namespace zvec::fts {

static Result<ailego::JsonObject> parse_config(const FtsIndexParams &params) {
  // Parse extra_params JSON string into a JsonObject.
  // Empty string is treated as an empty object; malformed JSON fails.
  ailego::JsonObject extra_json;
  if (!params.extra_params.empty()) {
    ailego::JsonValue parsed;
    if (!parsed.parse(params.extra_params.c_str())) {
      return tl::make_unexpected(Status::InvalidArgument(
          "TokenizerFactory: failed to parse extra_params JSON: ",
          params.extra_params));
    }
    if (!parsed.is_object()) {
      return tl::make_unexpected(Status::InvalidArgument(
          "TokenizerFactory: extra_params is not a JSON object: ",
          params.extra_params));
    }
    extra_json = parsed.as_object();
  }

  return extra_json;
}

Result<std::shared_ptr<void>> TokenizerFactory::reserve_model(
    const FtsIndexParams &params) {
  if (params.tokenizer_name != "jieba") {
    return std::shared_ptr<void>{};
  }
  auto config = parse_config(params);
  if (!config) {
    return tl::make_unexpected(config.error());
  }
  return JiebaTokenizer::reserve_model(
      TokenizerPipelineManager::make_key(params), *config);
}

Result<TokenizerPipelinePtr> TokenizerFactory::create(
    const FtsIndexParams &params) {
  auto config = parse_config(params);
  if (!config) {
    return tl::make_unexpected(config.error());
  }
  TokenizerPtr tokenizer;
  auto status = create_tokenizer(params.tokenizer_name, *config, &tokenizer);
  if (!status.ok()) {
    return tl::make_unexpected(std::move(status));
  }

  std::vector<TokenFilterPtr> filters;
  for (const auto &filter_name : params.filters) {
    TokenFilterPtr filter = create_filter(filter_name);
    if (!filter) {
      return tl::make_unexpected(Status::InvalidArgument(
          "TokenizerFactory: unknown filter name: ", filter_name));
    }
    if (!filter->init(*config)) {
      return tl::make_unexpected(Status::InvalidArgument(
          "TokenizerFactory: failed to init filter: ", filter_name));
    }
    filters.push_back(std::move(filter));
  }

  return std::make_shared<TokenizerPipeline>(std::move(tokenizer),
                                             std::move(filters));
}

std::vector<Token> TokenizerPipeline::process(const std::string &text) const {
  std::vector<Token> tokens = tokenizer_->tokenize(text);
  for (const auto &filter : filters_) {
    tokens = filter->filter(std::move(tokens));
  }
  return tokens;
}

Status TokenizerPipeline::validate_streaming(size_t workspace_bytes) const {
  if (!tokenizer_->supports_streaming()) {
    return Status::NotSupported("Unbounded FTS tokenizer: ",
                                tokenizer_->name());
  }
  for (const auto &filter : filters_) {
    auto status = filter->validate_streaming();
    if (!status.ok()) {
      return status;
    }
  }
  const size_t filter_bytes =
      filters_.empty() ? 0 : std::min<size_t>(workspace_bytes / 2, 1024 * 1024);
  if ((!filters_.empty() && filter_bytes < 8192) ||
      tokenizer_->minimum_workspace() > workspace_bytes - filter_bytes) {
    return {StatusCode::RESOURCE_EXHAUSTED,
            std::string("Insufficient FTS analyzer workspace for ") +
                tokenizer_->name()};
  }
  return Status::OK();
}

Status TokenizerPipeline::analyze_to(std::string_view text,
                                     const TokenSink &sink,
                                     size_t workspace_bytes) const {
  if (!sink || text.size() > UINT32_MAX) {
    return Status::InvalidArgument(
        "FTS token offset exceeds uint32 or missing sink");
  }
  auto status = validate_streaming();
  if (!status.ok()) {
    return status;
  }
  if (filters_.empty()) {
    return tokenizer_->analyze_to(text, sink, workspace_bytes);
  }
  // Unicode compatibility decomposition expands one code point to at most
  // 18 code points. Include output capacity growth, the live input token,
  // and fixed utf8proc scratch for decoding a single code point.
  const size_t filter_bytes =
      std::min<size_t>(workspace_bytes / 2, 1024 * 1024);
  if (filter_bytes < 4096) {
    return {StatusCode::RESOURCE_EXHAUSTED, "FTS filter workspace too small"};
  }
  if (filters_.size() == 1) {
    if (const auto *lower =
            dynamic_cast<const LowercaseTokenFilter *>(filters_[0].get())) {
      std::string scratch;
      return tokenizer_->analyze_to(
          text,
          [&](std::string_view word, uint32_t offset, uint32_t position) {
            return lower->analyze_one(word, offset, position, filter_bytes,
                                      &scratch, sink);
          },
          workspace_bytes - filter_bytes);
    }
  }
  std::vector<Token> one;
  one.reserve(1);
  return tokenizer_->analyze_to(
      text,
      [&](std::string_view word, uint32_t offset, uint32_t position) {
        if (word.size() > (filter_bytes - 4096) / 64) {
          return Status{StatusCode::RESOURCE_EXHAUSTED,
                        "FTS filter token exceeds workspace"};
        }
        one.push_back({std::string(word), offset, position});
        for (const auto &filter : filters_) {
          if (one.front().text.size() > (filter_bytes - 4096) / 64) {
            return Status{StatusCode::RESOURCE_EXHAUSTED,
                          "FTS filter token exceeds workspace"};
          }
          auto filtered = filter->filter_one(&one, filter_bytes / 2);
          if (!filtered.ok()) {
            return filtered;
          }
          if (one.empty()) {
            return Status::OK();
          }
        }
        auto result = sink(one.front().text, offset, position);
        one.clear();
        return result;
      },
      workspace_bytes - filter_bytes);
}

Status TokenizerFactory::create_tokenizer(const std::string &tokenizer_name,
                                          const ailego::JsonObject &extra_json,
                                          TokenizerPtr *tokenizer) {
  if (tokenizer_name.empty() || tokenizer_name == "standard") {
    *tokenizer = std::make_shared<StandardTokenizer>();
  } else if (tokenizer_name == "ngram") {
    *tokenizer = std::make_shared<NGramTokenizer>();
  } else if (tokenizer_name == "jieba") {
    *tokenizer = std::make_shared<JiebaTokenizer>();
  } else if (tokenizer_name == "whitespace") {
    *tokenizer = std::make_shared<WhitespaceTokenizer>();
  } else {
    return Status::InvalidArgument("TokenizerFactory: unknown tokenizer name: ",
                                   tokenizer_name);
  }

  auto status = (*tokenizer)->init(extra_json);
  if (!status.ok()) {
    tokenizer->reset();
    return status;
  }
  return Status::OK();
}

TokenFilterPtr TokenizerFactory::create_filter(const std::string &filter_name) {
  if (filter_name == "lowercase") {
    return std::make_shared<LowercaseTokenFilter>();
  } else if (filter_name == "ascii_folding") {
    return std::make_shared<AsciiFoldingTokenFilter>();
  } else if (filter_name == "stemmer") {
    // The stemmer filter uses Snowball and defaults to "english" unless
    // extra_params overrides stemmer_lang.
    return std::make_shared<StemmerTokenFilter>();
  }
  LOG_ERROR("[TokenizerFactory] unknown filter name: %s", filter_name.c_str());
  return nullptr;
}

}  // namespace zvec::fts
