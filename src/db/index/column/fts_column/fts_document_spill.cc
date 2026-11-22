// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include "fts_document_spill.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include "db/common/file_helper.h"
#include "db/common/typedef.h"
#include "db/index/common/manifest_codec.h"
#include "tokenizer/tokenizer_factory.h"
#include "fts_pipeline.h"
#include "fts_run.h"
#include "fts_stream.h"

namespace zvec::fts {
namespace {

// Fixed-width, process-private sort records. Short keys are entirely inline.
// Long keys go into the shared byte spool and comparisons read small windows.
struct TokenRef {
  uint64_t offset{0};
  uint32_t length{0}, position{0};
  std::array<char, 16> prefix{};
};
static_assert(sizeof(TokenRef) == 32);

template <class T>
std::string_view Bytes(const T &pod) {
  static_assert(std::is_trivially_copyable_v<T>);
  return {reinterpret_cast<const char *>(&pod), sizeof(T)};
}

class Window {
 public:
  explicit Window(FtsSpool &file) : file_(file) {}
  template <class T>
  Result<T> read(uint64_t offset) {
    if (!format::ValidRange(offset, sizeof(T), file_.size())) {
      return tl::make_unexpected(
          Status::InternalError("Invalid FTS document spool"));
    }
    if (offset < start_ ||
        !format::ValidRange(offset - start_, sizeof(T), page_.size())) {
      auto page =
          file_.read(offset, std::min<uint64_t>(4096, file_.size() - offset));
      if (!page) {
        return tl::make_unexpected(page.error());
      }
      start_ = offset;
      page_ = std::move(*page);
    }
    T value;
    std::memcpy(&value, page_.data() + (offset - start_), sizeof(T));
    return value;
  }

 private:
  FtsSpool &file_;
  std::string page_;
  uint64_t start_{0};
};

struct Store {
  // Last owner closes spools before removing their directory.
  FtsFileOwner::Pointer directory;
  std::shared_ptr<FtsSpoolWorkspace> workspace;
  std::unique_ptr<FtsSpool> keys, groups;
  Result<std::string> key(const TokenRef &ref, uint64_t at, size_t n) {
    if (!format::ValidRange(at, n, ref.length)) {
      return tl::make_unexpected(
          Status::InvalidArgument("Invalid spilled FTS term range"));
    }
    if (ref.length <= ref.prefix.size()) {
      return std::string(ref.prefix.data() + at, n);
    }
    return keys->read(ref.offset + at, n);
  }
  Result<int> compare(const TokenRef &a, const TokenRef &b) {
    const size_t n = std::min(a.length, b.length);
    const size_t prefix = std::min(n, a.prefix.size());
    int result = std::memcmp(a.prefix.data(), b.prefix.data(), prefix);
    for (size_t i = prefix; !result && i < n;) {
      size_t step = std::min<size_t>(4096, n - i);
      auto x = key(a, i, step);
      auto y = key(b, i, step);
      if (!x || !y) {
        return tl::make_unexpected(!x ? x.error() : y.error());
      }
      result = std::memcmp(x->data(), y->data(), step);
      i += step;
    }
    if (!result) {
      result = (a.length > b.length) - (a.length < b.length);
    }
    return result;
  }
};

// Two-way stable merges preserve analyzer emission order for equal keys.
// Every sorted fragment except the tail contains exactly width records, so
// fragment locations are arithmetic and need no growing directory.
class Sorter {
 public:
  Sorter(Store &store, size_t capacity, std::unique_ptr<FtsSpool> initial)
      : store_(store), capacity_(capacity), input_(std::move(initial)) {
    rows_.reserve(capacity);
    work_.resize(capacity);
  }
  Status add(std::string_view word, uint32_t position) {
    if (count_ == UINT32_MAX || word.size() > UINT32_MAX) {
      return Status::InvalidArgument("Spilled FTS field exceeds uint32");
    }
    TokenRef ref;
    ref.length = static_cast<uint32_t>(word.size());
    ref.position = position;
    if (!word.empty()) {
      std::memcpy(ref.prefix.data(), word.data(),
                  std::min(word.size(), ref.prefix.size()));
    }
    if (word.size() > ref.prefix.size()) {
      ref.offset = store_.keys->size();
      auto status = store_.keys->append(word);
      CHECK_RETURN_STATUS(status);
    }
    ++count_;
    rows_.push_back(ref);
    return rows_.size() == capacity_ ? flush_chunk() : Status::OK();
  }
  uint32_t count() const {
    return count_;
  }

  Status finish() {
    auto status = flush_chunk();
    CHECK_RETURN_STATUS(status);
    for (uint64_t width = capacity_; width < count_; width *= 2) {
      auto output = FtsSpool::Create(store_.workspace);
      if (!output) {
        return output.error();
      }
      Window left(*input_), right(*input_);
      for (uint64_t begin = 0; begin < count_; begin += width * 2) {
        uint64_t a = begin, mid = std::min<uint64_t>(begin + width, count_);
        uint64_t b = mid, end = std::min<uint64_t>(mid + width, count_);
        while (a < mid || b < end) {
          TokenRef l, r;
          if (a < mid) {
            auto next = left.read<TokenRef>(a * sizeof(TokenRef));
            if (!next) {
              return next.error();
            }
            l = *next;
          }
          if (b < end) {
            auto next = right.read<TokenRef>(b * sizeof(TokenRef));
            if (!next) {
              return next.error();
            }
            r = *next;
          }
          bool use_left = b == end;
          if (a < mid && b < end) {
            auto cmp = store_.compare(l, r);
            if (!cmp) {
              return cmp.error();
            }
            use_left = *cmp <= 0;
          }
          status = (*output)->append(Bytes(use_left ? l : r));
          CHECK_RETURN_STATUS(status);
          if (use_left) {
            ++a;
          } else {
            ++b;
          }
        }
      }
      input_ = std::move(*output);
    }
    Window input(*input_);
    TokenRef previous;
    uint32_t tf = 0;
    uint64_t tf_offset = 0;
    for (uint64_t i = 0; i < count_; ++i) {
      auto next = input.read<TokenRef>(i * sizeof(TokenRef));
      if (!next) {
        return next.error();
      }
      bool new_key = !tf;
      if (tf) {
        auto cmp = store_.compare(previous, *next);
        if (!cmp) {
          return cmp.error();
        }
        new_key = *cmp != 0;
      }
      if (new_key) {
        if (tf) {
          status = store_.groups->overwrite(tf_offset, Bytes(tf));
          CHECK_RETURN_STATUS(status);
        }
        previous = *next;
        tf = 0;
        status = store_.groups->append(Bytes(previous));
        CHECK_RETURN_STATUS(status);
        tf_offset = store_.groups->size();
        status = store_.groups->append(Bytes(tf));
        CHECK_RETURN_STATUS(status);
      }
      ++tf;
      status = store_.groups->append(Bytes(next->position));
      CHECK_RETURN_STATUS(status);
    }
    return tf ? store_.groups->overwrite(tf_offset, Bytes(tf)) : Status::OK();
  }

 private:
  Status flush_chunk() {
    const auto n = rows_.size();
    TokenRef *from = rows_.data(), *to = work_.data();
    for (size_t width = 1; width < n; width *= 2) {
      for (size_t begin = 0; begin < n; begin += width * 2) {
        size_t a = begin, mid = std::min(n, begin + width);
        size_t b = mid, end = std::min(n, mid + width);
        for (size_t out = begin; out < end; ++out) {
          bool use_left = b == end;
          if (a < mid && b < end) {
            auto cmp = store_.compare(from[a], from[b]);
            if (!cmp) {
              return cmp.error();
            }
            use_left = *cmp <= 0;
          }
          to[out] = use_left ? from[a++] : from[b++];
        }
      }
      std::swap(from, to);
    }
    if (n) {
      auto status = input_->append(
          {reinterpret_cast<const char *>(from), n * sizeof(TokenRef)});
      CHECK_RETURN_STATUS(status);
    }
    rows_.clear();
    return Status::OK();
  }
  Store &store_;
  size_t capacity_;
  uint32_t count_{0};
  std::unique_ptr<FtsSpool> input_;
  std::vector<TokenRef> rows_, work_;
};

class Positions final : public FtsReadPosition {
 public:
  Positions(std::shared_ptr<Store> store, uint64_t begin, uint32_t count)
      : store_(std::move(store)),
        input_(*store_->groups),
        at_(begin),
        remaining_(count) {}
  Result<std::optional<uint32_t>> next() override {
    if (!remaining_) {
      current_.reset();
    } else {
      auto pos = input_.read<uint32_t>(at_);
      if (!pos) {
        return tl::make_unexpected(pos.error());
      }
      current_ = *pos;
      at_ += sizeof(uint32_t);
      --remaining_;
    }
    return current_;
  }
  Result<std::optional<uint32_t>> seek_ge(uint32_t target) override {
    while (!current_ || *current_ < target) {
      auto position = next();
      if (!position || !*position) {
        return position;
      }
    }
    return current_;
  }

 private:
  std::shared_ptr<Store> store_;
  Window input_;
  uint64_t at_;
  uint32_t remaining_;
  std::optional<uint32_t> current_;
};

class Posting final : public FtsReadPosting {
 public:
  explicit Posting(FtsReadPayload payload) : payload_(payload) {}
  Result<uint32_t> next() override {
    if (started_) {
      eof_ = true;
    }
    started_ = true;
    return eof_ ? UINT32_MAX : 0;
  }
  Result<uint32_t> advance(uint32_t target) override {
    eof_ = eof_ || target != 0;
    started_ = true;
    return eof_ ? UINT32_MAX : 0;
  }
  Result<FtsReadPayload> payload() override {
    if (!started_ || eof_) {
      return tl::make_unexpected(
          Status::InvalidArgument("No current spilled posting"));
    }
    return payload_;
  }
  Result<Bound> bound(uint32_t, float) override {
    return Bound{std::numeric_limits<float>::infinity(), UINT32_MAX};
  }

 private:
  FtsReadPayload payload_;
  bool started_{false}, eof_{false};
};

class Term final : public FtsReadTerm {
 public:
  Term(std::shared_ptr<Store> store, uint64_t offset, uint32_t tf,
       uint32_t length)
      : store_(std::move(store)), offset_(offset), payload_{tf, length} {}
  uint64_t df() const override {
    return 1;
  }
  float max_score(float) const override {
    return std::numeric_limits<float>::infinity();
  }
  Result<FtsReadPosting::Ptr> postings() const override {
    return std::make_unique<Posting>(payload_);
  }
  Result<FtsReadPosition::Ptr> positions(uint32_t doc) const override {
    if (doc != 0) {
      return tl::make_unexpected(
          Status::InvalidArgument("Spill row must be zero"));
    }
    return std::make_unique<Positions>(store_, offset_, payload_.tf);
  }

 private:
  std::shared_ptr<Store> store_;
  uint64_t offset_;
  FtsReadPayload payload_;
};

class Terms final : public FtsSourceTerms {
 public:
  Terms(std::shared_ptr<Store> store, uint64_t begin, uint64_t end,
        uint32_t length)
      : store_(std::move(store)),
        input_(*store_->groups),
        at_(begin),
        end_(end),
        length_(length) {}
  Result<std::optional<FtsSourceTerm>> next() override {
    if (at_ == end_) {
      return std::optional<FtsSourceTerm>{};
    }
    auto ref = input_.read<TokenRef>(at_);
    auto tf = input_.read<uint32_t>(at_ + sizeof(TokenRef));
    if (!ref || !tf) {
      return tl::make_unexpected(!ref ? ref.error() : tf.error());
    }
    at_ += sizeof(TokenRef) + sizeof(uint32_t);
    const uint64_t bytes = uint64_t{*tf} * sizeof(uint32_t);
    if (!*tf || !format::ValidRange(at_, bytes, end_)) {
      return tl::make_unexpected(
          Status::InternalError("Invalid FTS spill positions"));
    }
    FtsSourceTerm term;
    term.length = ref->length;
    term.bytes = [store = store_, ref = *ref](uint64_t at, size_t n) {
      return store->key(ref, at, n);
    };
    term.reader = std::make_shared<Term>(store_, at_, *tf, length_);
    at_ += bytes;
    return std::optional<FtsSourceTerm>{std::move(term)};
  }

 private:
  std::shared_ptr<Store> store_;
  Window input_;
  uint64_t at_, end_;
  uint32_t length_;
};

class FieldSource final : public FtsFieldSource {
 public:
  FieldSource(std::shared_ptr<Store> store, uint64_t begin, bool present,
              uint32_t length, const FieldSchema &schema)
      : store_(std::move(store)),
        begin_(begin),
        end_(store_->groups->size()),
        stats_{1, present, length, present} {
    metadata_.name = schema.name();
    ManifestCodec::EncodeIndexParams(schema.index_params().get(),
                                     &metadata_.analysis_identity);
    metadata_.total_tokens = length;
    metadata_.present_count = metadata_.scoring_document_count = present;
  }
  const format::FieldMetadata &metadata() const override {
    return metadata_;
  }
  const FtsSourceStatistics &statistics() const override {
    return stats_;
  }
  uint64_t term_workspace_limit() const override {
    return 32768;
  }
  Result<FtsDocumentLength> document(uint32_t doc) override {
    if (doc != 0) {
      return tl::make_unexpected(
          Status::InvalidArgument("Spill row must be zero"));
    }
    return FtsDocumentLength{stats_.present != 0,
                             static_cast<uint32_t>(stats_.total_tokens)};
  }
  Result<FtsSourceTerms::Ptr> terms() const override {
    return std::make_unique<Terms>(store_, begin_, end_, stats_.total_tokens);
  }

 private:
  std::shared_ptr<Store> store_;
  uint64_t begin_, end_;
  FtsSourceStatistics stats_;
  format::FieldMetadata metadata_;
};
}  // namespace

Result<std::vector<std::vector<FtsFieldSource::Ptr>>> PrepareSpilledFtsDocument(
    const FieldSchemaPtrList &fields,
    const std::vector<std::optional<std::string_view>> &text,
    const std::string &segment_path, uint32_t block,
    const FtsRetirementQueue::Pointer &retirement, size_t prepare_bytes,
    uint64_t disk_bytes) {
  if (!retirement || fields.size() != text.size() || fields.empty() ||
      fields.size() > format::kMaxFields) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS document spill configuration"));
  }
  // Two cached inputs and key comparison windows, stdio buffers, per-field
  // capture and one analyzer reserve. The remaining space holds both arrays.
  auto metadata = detail::FtsAnalysisMetadataBytes(
      fields, 65536 + fields.size() * 1024, prepare_bytes / 2);
  if (!metadata) {
    return tl::make_unexpected(metadata.error());
  }
  const size_t overhead = *metadata;
  if (prepare_bytes / 2 - overhead <= 2 * sizeof(TokenRef)) {
    return tl::make_unexpected(Status{StatusCode::RESOURCE_EXHAUSTED,
                                      "FTS spill workspace too small"});
  }
  const size_t capacity = std::min<size_t>(
      UINT32_MAX, (prepare_bytes / 2 - overhead) / (2 * sizeof(TokenRef)));
  const auto path =
      FileHelper::MakeFtsRunPath(segment_path, block, 1, NewFtsIdentity()) +
      ".reserved";
  std::error_code error;
  if (!std::filesystem::create_directory(ailego::FileHelper::PathFromUtf8(path),
                                         error)) {
    return tl::make_unexpected(Status{StatusCode::UNAVAILABLE,
                                      "Cannot reserve FTS document workspace"});
  }
  auto store = std::make_shared<Store>();
  store->directory = retirement->track(path, true, true);
  store->workspace = std::make_shared<FtsSpoolWorkspace>(path, disk_bytes, 4);
  auto keys = FtsSpool::Create(store->workspace);
  auto groups = FtsSpool::Create(store->workspace);
  if (!keys || !groups) {
    return tl::make_unexpected(!keys ? keys.error() : groups.error());
  }
  store->keys = std::move(*keys);
  store->groups = std::move(*groups);
  std::vector<std::vector<FtsFieldSource::Ptr>> sources(fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto begin = store->groups->size();
    uint32_t length = 0;
    if (text[i]) {
      auto params = std::dynamic_pointer_cast<zvec::FtsIndexParams>(
          fields[i]->index_params());
      if (!params) {
        return tl::make_unexpected(
            Status::InvalidArgument("Missing FTS analyzer"));
      }
      auto pipeline = detail::AcquireFtsWritingPipeline(*params);
      if (!pipeline) {
        return tl::make_unexpected(pipeline.error());
      }
      auto initial = FtsSpool::Create(store->workspace);
      if (!initial) {
        return tl::make_unexpected(initial.error());
      }
      Sorter sorter(*store, capacity, std::move(*initial));
      auto status = (*pipeline)->analyze_to(
          *text[i],
          [&](std::string_view key, uint32_t, uint32_t pos) {
            return sorter.add(key, pos);
          },
          prepare_bytes / 2);
      CHECK_RETURN_STATUS_EXPECTED(status);
      status = sorter.finish();
      CHECK_RETURN_STATUS_EXPECTED(status);
      length = sorter.count();
    }
    sources[i].push_back(std::make_shared<FieldSource>(
        store, begin, text[i].has_value(), length, *fields[i]));
  }
  return sources;
}
}  // namespace zvec::fts
