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

#include "jieba_tokenizer.h"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <zvec/ailego/logger/logger.h>
#include "jieba_embedded_dict.h"

namespace zvec::fts {

static std::atomic<size_t> model_input_peak{0};

size_t JiebaModelPeakInputBytes() {
  return model_input_peak.load(std::memory_order_relaxed);
}

static std::string get_string_or_default(const ailego::JsonObject &config,
                                         const char *key,
                                         const std::string &default_value) {
  auto val = config[key];
  if (val.is_string()) {
    std::string result = val.as_string().c_str();
    if (!result.empty()) {
      return result;
    }
  }
  return default_value;
}

// Priority: per-field config > ZVEC_JIEBA_DICT_DIR > embedded defaults.
static std::string resolve_jieba_dict_dir(const ailego::JsonObject &config) {
  std::string dir = get_string_or_default(config, "jieba_dict_dir", "");
  if (!dir.empty()) {
    return dir;
  }
  if (const char *env = std::getenv("ZVEC_JIEBA_DICT_DIR"); env && *env) {
    return env;
  }
  return {};
}

Result<std::shared_ptr<void>> JiebaTokenizer::reserve_model(
    const std::string &key, const ailego::JsonObject &config) {
  // Admission units are model source bytes, independent of document memory.
  // cppjieba builds at most one trie node/map edge per input rune, one record
  // per dictionary line and one probability per HMM input entry. A fixed
  // charge bounds the count of tiny pipelines and external path cache keys.
  constexpr size_t limit = 64u << 20;
  constexpr size_t entry_charge = 64u << 10;
  struct Entry {
    std::weak_ptr<void> owner;
    size_t bytes;
  };
  static std::mutex mutex;
  static std::map<std::string, Entry> live;
  static std::map<std::string, size_t> cached_external_dicts;
  static size_t permanent =
      embedded_jieba_dict_input_bytes() + embedded_jieba_hmm_input_bytes();
  std::lock_guard lock(mutex);
  auto found = live.find(key);
  if (found != live.end()) {
    if (auto pin = found->second.owner.lock()) {
      return pin;
    }
  }
  size_t used = permanent;
  for (auto it = live.begin(); it != live.end();) {
    if (it->second.owner.expired()) {
      it = live.erase(it);
    } else {
      used += it->second.bytes;
      ++it;
    }
  }
  auto full = [] {
    return Status{StatusCode::RESOURCE_EXHAUSTED,
                  "FTS Jieba model input budget exhausted"};
  };
  if (key.size() > entry_charge || entry_charge > limit - used) {
    return tl::make_unexpected(full());
  }
  size_t charge = entry_charge;
  auto append = [&](size_t bytes) {
    if (bytes > limit - used - charge) {
      return false;
    }
    charge += bytes;
    return true;
  };
  auto file_size = [&](const std::string &path) -> Result<size_t> {
    std::ifstream stream;
    cppjieba::OpenInputFile(stream, path);
    stream.seekg(0, std::ios::end);
    const std::streamoff bytes = stream.tellg();
    if (bytes < 0) {
      return tl::make_unexpected(Status::InvalidArgument(
          "Cannot measure FTS Jieba model '", path, "'"));
    }
    if (static_cast<uint64_t>(bytes) > limit) {
      return tl::make_unexpected(full());
    }
    return static_cast<size_t>(bytes);
  };
  const auto dir = resolve_jieba_dict_dir(config);
  const auto mode = get_string_or_default(config, "cut_mode", "search");
  std::string base_path;
  size_t cache_charge = 0;
  if (mode != "hmm") {
    size_t dict_bytes = embedded_jieba_dict_input_bytes();
    if (!dir.empty()) {
      base_path = dir + "/jieba.dict.utf8";
      auto cached = cached_external_dicts.find(base_path);
      if (cached == cached_external_dicts.end()) {
        auto bytes = file_size(base_path);
        if (!bytes) {
          return tl::make_unexpected(bytes.error());
        }
        dict_bytes = *bytes;
        // cppjieba's external base dictionary cache holds its entries for
        // process life. Retain their admission even after the last pipeline.
        cache_charge = dict_bytes + entry_charge;
        if (!append(cache_charge)) {
          return tl::make_unexpected(full());
        }
      } else {
        dict_bytes = cached->second;
      }
    }
    if (!append(dict_bytes)) {
      return tl::make_unexpected(full());
    }
    auto users = get_string_or_default(config, "user_dict_path", "");
    if (!users.empty()) {
      for (const auto &path : limonp::Split(users, "|;")) {
        auto bytes = file_size(path);
        if (!bytes) {
          return tl::make_unexpected(bytes.error());
        }
        if (!append(*bytes)) {
          return tl::make_unexpected(full());
        }
      }
    }
  }
  if (mode != "full") {
    size_t hmm_bytes = embedded_jieba_hmm_input_bytes();
    if (!dir.empty()) {
      auto n = file_size(dir + "/hmm_model.utf8");
      if (!n) {
        return tl::make_unexpected(n.error());
      }
      hmm_bytes = *n;
    }
    if (!append(hmm_bytes)) {
      return tl::make_unexpected(full());
    }
  }
  if (cache_charge) {
    cached_external_dicts.emplace(base_path, cache_charge - entry_charge);
    permanent += cache_charge;
  }
  auto pin = std::make_shared<char>();
  live.emplace(key, Entry{pin, charge - cache_charge});
  model_input_peak.store(std::max(used + charge, JiebaModelPeakInputBytes()),
                         std::memory_order_relaxed);
  return std::shared_ptr<void>(std::move(pin));
}

// cppjieba aborts when a file cannot be opened. Validate configured paths
// before constructing its dictionary or model, preserving UTF-8 path support.
static Status check_jieba_file(const std::string &path, bool allow_empty) {
  std::ifstream stream;
  cppjieba::OpenInputFile(stream, path);
  if (!stream.is_open()) {
    return Status::InvalidArgument("JiebaTokenizer: cannot read file '", path,
                                   "'");
  }
  auto first = stream.peek();
  if (stream.bad() ||
      (!allow_empty && first == std::ifstream::traits_type::eof())) {
    return Status::InvalidArgument(
        "JiebaTokenizer: cannot read nonempty file '", path, "'");
  }
  return Status::OK();
}

Status JiebaTokenizer::init(const ailego::JsonObject &config) {
  reset();
  longest_word_ = cppjieba::MAX_WORD_LENGTH;

  std::string user_dict_path =
      get_string_or_default(config, "user_dict_path", "");

  std::string mode_str = get_string_or_default(config, "cut_mode", "search");
  if (mode_str == "search") {
    cut_mode_ = CutMode::kSearch;
  } else if (mode_str == "mix") {
    cut_mode_ = CutMode::kMix;
  } else if (mode_str == "full") {
    cut_mode_ = CutMode::kFull;
  } else if (mode_str == "hmm") {
    cut_mode_ = CutMode::kHmm;
  } else {
    return Status::InvalidArgument("JiebaTokenizer: unknown cut_mode '",
                                   mode_str, "'");
  }

  bool needs_dict = cut_mode_ != CutMode::kHmm;
  bool needs_model = cut_mode_ != CutMode::kFull;

  std::string dict_dir = resolve_jieba_dict_dir(config);

  std::string dict_path = needs_dict ? dict_dir + "/jieba.dict.utf8" : "";
  std::string model_path = needs_model ? dict_dir + "/hmm_model.utf8" : "";

  if (!dict_dir.empty()) {
    if (needs_dict) {
      auto status = check_jieba_file(dict_path, false);
      if (!status.ok()) {
        return status;
      }
    }
    if (needs_model) {
      auto status = check_jieba_file(model_path, false);
      if (!status.ok()) {
        return status;
      }
    }
  }
  if (needs_dict && !user_dict_path.empty()) {
    for (const auto &path : limonp::Split(user_dict_path, "|;")) {
      auto status = check_jieba_file(path, true);
      if (!status.ok()) {
        return status;
      }
    }
  }

  try {
    if (needs_dict) {
      if (dict_dir.empty()) {
        auto dict = get_embedded_jieba_dict();
        if (!dict) {
          return dict.error();
        }
        dict_trie_ =
            std::make_unique<cppjieba::DictTrie>(**dict, user_dict_path);
        if (user_dict_path.empty()) {
          size_t longest = 1;
          for (const auto &entry : *(*dict)->node_infos) {
            longest = std::max(longest, entry.word.size());
          }
          longest_word_ = std::min(longest, cppjieba::MAX_WORD_LENGTH);
        }
      } else {
        dict_trie_ =
            std::make_unique<cppjieba::DictTrie>(dict_path, user_dict_path);
      }
    }
    if (needs_model) {
      if (dict_dir.empty()) {
        auto model = get_embedded_jieba_hmm_model();
        if (!model) {
          reset();
          return model.error();
        }
        hmm_model_ = *model;
      } else {
        hmm_model_ = std::make_shared<const cppjieba::HMMModel>(model_path);
      }
    }
    switch (cut_mode_) {
      case CutMode::kSearch:
        query_seg_ = std::make_unique<cppjieba::QuerySegment>(dict_trie_.get(),
                                                              hmm_model_.get());
        break;
      case CutMode::kMix:
        mix_seg_ = std::make_unique<cppjieba::MixSegment>(dict_trie_.get(),
                                                          hmm_model_.get());
        break;
      case CutMode::kFull:
        full_seg_ = std::make_unique<cppjieba::FullSegment>(dict_trie_.get());
        break;
      case CutMode::kHmm:
        hmm_seg_ = std::make_unique<cppjieba::HMMSegment>(hmm_model_.get());
        break;
    }
  } catch (const std::exception &e) {
    LOG_ERROR("JiebaTokenizer init failed: %s", e.what());
    reset();
    return Status::InvalidArgument("JiebaTokenizer init failed: ", e.what());
  }

  initialized_ = true;
  LOG_INFO("JiebaTokenizer init success. dict_dir[%s] cut_mode[%s]",
           dict_dir.c_str(), mode_str.c_str());
  return Status::OK();
}

JiebaTokenizer::~JiebaTokenizer() = default;

void JiebaTokenizer::reset() {
  query_seg_.reset();
  mix_seg_.reset();
  full_seg_.reset();
  hmm_seg_.reset();
  dict_trie_.reset();
  hmm_model_.reset();
  initialized_ = false;
}

std::vector<Token> JiebaTokenizer::tokenize(const std::string &text) const {
  std::vector<Token> tokens;
  if (!initialized_ || text.empty()) {
    return tokens;
  }

  std::vector<cppjieba::Word> words;
  switch (cut_mode_) {
    case CutMode::kSearch:
      query_seg_->Cut(text, words, true);
      break;
    case CutMode::kMix:
      mix_seg_->Cut(text, words, true);
      break;
    case CutMode::kFull:
      full_seg_->Cut(text, words);
      break;
    case CutMode::kHmm:
      hmm_seg_->Cut(text, words);
      break;
  }

  tokens.reserve(words.size());
  // Position = output sequence index, not cppjieba's unicode_offset:
  // overlapping sub-words emitted after long parents share unicode_offset,
  // which breaks PhraseDocIterator's strict anchor+1 adjacency check.
  uint32_t seq = 0;
  for (const auto &word : words) {
    if (word.word.empty()) {
      continue;
    }
    Token token;
    token.text = word.word;
    token.offset = word.offset;
    token.position = seq++;
    tokens.push_back(std::move(token));
  }

  return tokens;
}

Status JiebaTokenizer::analyze_to(std::string_view text, const TokenSink &sink,
                                  size_t workspace_bytes) const {
  if (!initialized_ || text.size() > UINT32_MAX) {
    return Status::InvalidArgument("Invalid Jieba analyzer input");
  }
  auto decode = [&](size_t i) {
    return cppjieba::DecodeUTF8ToRune(text.data() + i, text.size() - i);
  };
  // PreFilter clears its entire rune array on a malformed trailing sequence.
  // Validate the input once before emitting earlier units to the sink.
  for (size_t i = 0; i < text.size();) {
    auto rune = decode(i);
    if (rune.len == 0) {
      return Status::OK();
    }
    i += rune.len;
  }
  auto separator = [](uint32_t cp) {
    // cppjieba::SPECIAL_SEPARATORS (space, tab, newline, Chinese comma/stop).
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xff0c ||
           cp == 0x3002;
  };
  auto exhausted = [] {
    return Status{StatusCode::RESOURCE_EXHAUSTED,
                  "Jieba natural segment exceeds analyzer workspace"};
  };
  using Range = cppjieba::WordRange;
  using Edge = std::pair<size_t, const cppjieba::DictUnit *>;
  // Per rune: input rune, one Dag (its first 16 edges are inline), MP/mix
  // scratch, four HMM states (path + weight + backtrace), growing result
  // ranges. Search emits at most three ranges per rune. Full output can
  // contain one range per matching edge and is charged separately below.
  constexpr size_t per_rune = sizeof(cppjieba::RuneStr) +
                              sizeof(cppjieba::Dag) +
                              4 * (sizeof(int) + sizeof(double)) +
                              2 * sizeof(size_t) + 14 * sizeof(Range) + 96;
  cppjieba::RuneStrArray runes;
  std::vector<Range> words;
  uint64_t position = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    size_t count = 0, end = offset;
    while (end < text.size()) {
      auto rune = decode(end);
      if (separator(rune.rune)) {
        if (count == 0) {
          end += rune.len;
          ++count;
        }
        break;
      }
      ++count;
      end += rune.len;
    }
    // Release old geometrically sized buffers before a larger allocation.
    // Cap retained capacity between units at the current unit's size.
    if (runes.capacity() != count) {
      cppjieba::RuneStrArray{}.swap(runes);
    }
    std::vector<Range>{}.swap(words);
    if (workspace_bytes < 4096 || count > (workspace_bytes - 4096) / per_rune) {
      return exhausted();
    }
    runes.clear();
    runes.reserve(count);
    for (size_t i = offset; i < end;) {
      auto r = decode(i);
      runes.emplace_back(r.rune, static_cast<uint32_t>(i), r.len,
                         static_cast<uint32_t>(runes.size()), 1);
      i += r.len;
    }
    if (dict_trie_) {
      // LocalVector doubles after its inline 16 edges. Include the old and
      // new capacities; full-mode ranges also grow geometrically. Use a
      // cheap upper bound for short sentences. For long sentences count
      // actual dictionary prefixes before cppjieba allocates its DAG.
      auto extra = [&](size_t edges) {
        size_t bytes = edges > limonp::LOCAL_VECTOR_BUFFER_SIZE
                           ? 3 * edges * sizeof(Edge)
                           : 0;
        if (cut_mode_ == CutMode::kFull) {
          bytes += 3 * edges * sizeof(Range);
        }
        return bytes;
      };
      size_t remaining = workspace_bytes - 4096 - count * per_rune;
      if (extra(std::min(count, longest_word_)) > remaining / count) {
        for (auto first = runes.cbegin(); first != runes.cend(); ++first) {
          size_t edges = 1;
          const size_t limit =
              std::min<size_t>(longest_word_, runes.cend() - first);
          for (size_t len = 2; len <= limit; ++len) {
            edges += dict_trie_->Find(first, first + len) != nullptr;
          }
          size_t bytes = extra(edges);
          if (bytes > remaining) {
            return exhausted();
          }
          remaining -= bytes;
        }
      }
    }
    switch (cut_mode_) {
      case CutMode::kSearch:
        query_seg_->Cut(runes.begin(), runes.end(), words, true);
        break;
      case CutMode::kMix:
        mix_seg_->Cut(runes.begin(), runes.end(), words, true);
        break;
      case CutMode::kFull:
        full_seg_->Cut(runes.begin(), runes.end(), words);
        break;
      case CutMode::kHmm:
        hmm_seg_->Cut(runes.begin(), runes.end(), words);
        break;
    }
    for (const auto &word : words) {
      if (position > UINT32_MAX) {
        return Status::InvalidArgument("Jieba token position exceeds uint32");
      }
      const auto begin = word.left->offset;
      const size_t bytes = size_t{word.right->offset} + word.right->len - begin;
      auto status = sink(text.substr(begin, bytes), begin,
                         static_cast<uint32_t>(position++));
      if (!status.ok()) {
        return status;
      }
    }
    offset = end;
  }
  return Status::OK();
}

}  // namespace zvec::fts
