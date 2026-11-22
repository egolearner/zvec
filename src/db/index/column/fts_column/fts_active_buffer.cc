// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include "fts_active_buffer.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <zvec/ailego/internal/platform.h>
#include "db/common/typedef.h"
#include "db/index/common/manifest_codec.h"
#include "tokenizer/tokenizer_factory.h"
#include "fts_pipeline.h"

namespace zvec::fts {
namespace {

Status Full() {
  return {StatusCode::RESOURCE_EXHAUSTED,
          "FTS active or document budget exhausted"};
}

// One non-growing allocation. Every provisional object is trivially
// destructible: rolling back a failed document only rewinds the frontier.
class Arena {
 public:
  explicit Arena(size_t size) : size_(size) {}
  bool allocate() {
    data_.reset(new (std::nothrow) char[size_]);
    return static_cast<bool>(data_);
  }
  void release() {
    data_.reset();
    used = sort_bytes = 0;
  }
  template <typename T>
  T *array(size_t count = 1) {
    static_assert(std::is_trivially_destructible_v<T>);
    const size_t padding = (0 - used) & (alignof(T) - 1);
    const size_t sort_padding = sort_bytes ? alignof(void *) - 1 : 0;
    const size_t available = size_ - used - sort_bytes - sort_padding;
    if (padding > available || count > (available - padding) / sizeof(T)) {
      return nullptr;
    }
    T *out = reinterpret_cast<T *>(data_.get() + used + padding);
    std::uninitialized_value_construct_n(out, count);
    used += padding + count * sizeof(T);
    return out;
  }
  // Reserve the pointer to be sorted at freeze, including final alignment.
  bool reserve_key() {
    if (size_ - used - sort_bytes < sizeof(void *) * 2) {
      return false;
    }
    sort_bytes += sizeof(void *);
    return true;
  }
  size_t used{0}, sort_bytes{0};

 private:
  std::unique_ptr<char[]> data_;
  size_t size_;
};

struct Positions {
  Positions *next{nullptr};
  std::array<uint32_t, 8> values{};
};

struct Posting {
  Posting *next{nullptr};
  Positions *extra{nullptr};
  uint32_t row{0}, tf{0}, first{0};
};

struct Entry {
  Entry *collision{nullptr};
  std::string_view key;
  size_t hash{0};
  uint32_t field{0}, df{0};
  Posting *head{nullptr}, *tail{nullptr};
};

struct RowLocation {
  uint32_t block, within;
  explicit RowLocation(uint32_t row) {
    // Directory slot n covers 2^n rows starting at 2^n - 1.
    uint32_t value = row + 1;
    block = 31u - static_cast<uint32_t>(ailego_clz32(value));
    within = value - (uint32_t{1} << block);
  }
};

struct Field {
  format::FieldMetadata metadata;
  TokenizerPipelinePtr pipeline;
  FtsSourceStatistics stats;
  std::array<FtsDocumentLength *, 32> documents{};
  Entry **ordered{nullptr};
  uint32_t terms{0};

  FtsDocumentLength document(uint32_t row) const {
    RowLocation loc(row);
    return documents[loc.block][loc.within];
  }
};

struct Data {
  Data(size_t bytes, size_t prepare, std::shared_ptr<FtsWriteBudget> shared,
       FtsWriteBudget::Lease config)
      : budget(std::move(shared)),
        metadata_lease(std::move(config)),
        arena(bytes),
        active_limit(bytes),
        prepare_limit(prepare) {}
  Status start_arena() {
    if (table) {
      return Status::OK();
    }
    auto lease = budget->reserve(FtsWriteBudget::Part::Active, active_limit);
    if (!lease) {
      return lease.error();
    }
    if (!arena.allocate()) {
      return Full();
    }
    table = arena.array<Entry *>(buckets);
    arena_lease = std::move(*lease);
    return table ? Status::OK() : Full();
  }
  static size_t hash_key(uint32_t field, std::string_view key) {
    return std::hash<std::string_view>{}(key) ^ (field * size_t{2654435761u});
  }
  Entry *find(uint32_t field, std::string_view key, size_t hash) const {
    if (!table) {
      return nullptr;
    }
    for (auto *it = table[hash & (buckets - 1)]; it; it = it->collision) {
      if (it->hash == hash && it->field == field && it->key == key) {
        return it;
      }
    }
    return nullptr;
  }
  std::shared_ptr<FtsWriteBudget> budget;
  FtsWriteBudget::Lease metadata_lease, arena_lease;
  Arena arena;
  size_t active_limit, prepare_limit, buckets{0};
  Entry **table{nullptr};
  std::vector<Field> fields;
  uint32_t rows{0};
  bool preparing{false}, frozen{false};
};

class PositionCursor final : public FtsReadPosition {
 public:
  PositionCursor(std::shared_ptr<Data> data, Posting *posting)
      : data_(std::move(data)), posting_(posting), extra_(posting->extra) {}
  Result<std::optional<uint32_t>> next() override {
    if (count_ == posting_->tf) {
      current_.reset();
    } else if (count_++ == 0) {
      current_ = posting_->first;
    } else {
      auto index = (count_ - 2) % extra_->values.size();
      current_ = extra_->values[index];
      if (index + 1 == extra_->values.size()) {
        extra_ = extra_->next;
      }
    }
    return current_;
  }
  Result<std::optional<uint32_t>> seek_ge(uint32_t target) override {
    while ((!current_ || *current_ < target) && count_ < posting_->tf) {
      auto moved = next();
      if (!moved) {
        return moved;
      }
    }
    if (current_ && *current_ < target) {
      current_.reset();
    }
    return current_;
  }

 private:
  std::shared_ptr<Data> data_;
  Posting *posting_;
  Positions *extra_;
  uint32_t count_{0};
  std::optional<uint32_t> current_;
};

class PostingCursor final : public FtsReadPosting {
 public:
  PostingCursor(std::shared_ptr<Data> data, Entry *term)
      : data_(std::move(data)), field_(term->field), following_(term->head) {}
  Result<uint32_t> next() override {
    current_ = following_;
    if (current_) {
      following_ = current_->next;
    }
    return current_ ? current_->row : UINT32_MAX;
  }
  Result<uint32_t> advance(uint32_t target) override {
    while ((!current_ || current_->row < target) && following_) {
      auto moved = next();
      if (!moved) {
        return moved;
      }
    }
    if (current_ && current_->row < target) {
      current_ = nullptr;
    }
    return current_ ? current_->row : UINT32_MAX;
  }
  Result<FtsReadPayload> payload() override {
    if (!current_) {
      return tl::make_unexpected(
          Status::InvalidArgument("No current FTS posting"));
    }
    return FtsReadPayload{current_->tf,
                          data_->fields[field_].document(current_->row).length};
  }
  Result<FtsPostingBatch> next_batch() override {
    if (!following_) {
      current_ = nullptr;
      return FtsPostingBatch{};
    }
    if (!batch_) {
      batch_ = std::make_unique<Batch>();
    }
    const auto &field = data_->fields[field_];
    uint32_t count = 0;
    do {
      current_ = following_;
      following_ = current_->next;
      batch_->docs[count] = current_->row;
      batch_->tfs[count] = current_->tf;
      batch_->lengths[count] = field.document(current_->row).length;
      ++count;
    } while (following_ && count < FtsPostingBatch::kCapacity);
    return FtsPostingBatch{count, batch_->docs.data(), batch_->tfs.data(),
                           batch_->lengths.data()};
  }
  Result<FtsReadPosition::Ptr> current_positions(const FtsReadTerm &,
                                                 uint32_t row, bool) override {
    if (!current_ || current_->row != row) {
      return tl::make_unexpected(
          Status::InvalidArgument("No current FTS position list"));
    }
    return std::make_unique<PositionCursor>(data_, current_);
  }
  Result<Bound> bound(uint32_t, float) override {
    return Bound{std::numeric_limits<float>::infinity(), UINT32_MAX};
  }

 private:
  struct Batch {
    std::array<uint32_t, FtsPostingBatch::kCapacity> docs, tfs, lengths;
  };
  std::unique_ptr<Batch> batch_;
  std::shared_ptr<Data> data_;
  uint32_t field_;
  Posting *current_{nullptr}, *following_;
};

class ActiveTerm final : public FtsReadTerm {
 public:
  ActiveTerm(std::shared_ptr<Data> data, Entry *entry)
      : data_(std::move(data)), entry_(entry), hint_(entry->head) {}
  uint64_t df() const override {
    return entry_->df;
  }
  float max_score(float) const override {
    return std::numeric_limits<float>::infinity();
  }
  Result<FtsReadPosting::Ptr> postings() const override {
    return std::make_unique<PostingCursor>(data_, entry_);
  }
  Result<FtsReadPosition::Ptr> positions(uint32_t doc) const override {
    // Phrase and merge callers normally visit documents monotonically.
    // Cursor instances remain independent even for repeated phrase terms.
    Posting *posting = hint_.load(std::memory_order_relaxed);
    if (posting->row > doc) {
      posting = entry_->head;
    }
    while (posting && posting->row < doc) {
      posting = posting->next;
    }
    if (!posting || posting->row != doc) {
      return tl::make_unexpected(
          Status::InvalidArgument("No FTS positions for row"));
    }
    hint_.store(posting, std::memory_order_relaxed);
    return std::make_unique<PositionCursor>(data_, posting);
  }

 private:
  std::shared_ptr<Data> data_;
  Entry *entry_;
  mutable std::atomic<Posting *> hint_;
};

class ActiveReader final : public FtsSegmentReader {
 public:
  ActiveReader(std::shared_ptr<Data> data, uint32_t field)
      : data_(std::move(data)),
        field_(field),
        scorer_(std::make_shared<BM25Scorer>()) {
    const auto &stats = data_->fields[field_].stats;
    scorer_->update_stats(stats.scoring_documents, stats.total_tokens);
  }
  BM25ScorerPtr scorer() const override {
    return scorer_;
  }
  bool supports_batches(bool) const override {
    return true;
  }
  Result<FtsReadTerm::Ptr> lookup(std::string_view key) const override {
    auto *entry = data_->find(field_, key, Data::hash_key(field_, key));
    if (!entry) {
      return FtsReadTerm::Ptr{};
    }
    return std::make_shared<ActiveTerm>(data_, entry);
  }

 private:
  std::shared_ptr<Data> data_;
  uint32_t field_;
  BM25ScorerPtr scorer_;
};

class ActiveTerms final : public FtsSourceTerms {
 public:
  ActiveTerms(std::shared_ptr<Data> data, const Field &field)
      : data_(std::move(data)), field_(field) {}
  Result<std::optional<FtsSourceTerm>> next() override {
    if (index_ == field_.terms) {
      return std::optional<FtsSourceTerm>{};
    }
    auto *entry = field_.ordered[index_++];
    FtsSourceTerm term;
    term.length = entry->key.size();
    term.reader = std::make_shared<ActiveTerm>(data_, entry);
    term.bytes = [owner = data_, key = entry->key](
                     uint64_t begin, size_t bytes) -> Result<std::string> {
      if (!format::ValidRange(begin, bytes, key.size())) {
        return tl::make_unexpected(
            Status::InvalidArgument("Invalid active term range"));
      }
      return std::string(key.substr(begin, bytes));
    };
    return std::optional<FtsSourceTerm>{std::move(term)};
  }

 private:
  std::shared_ptr<Data> data_;
  const Field &field_;
  uint32_t index_{0};
};

class ActiveSource final : public FtsFrozenFieldSource {
 public:
  ActiveSource(std::shared_ptr<Data> data, uint32_t field)
      : data_(std::move(data)), field_(data_->fields[field]) {}
  const FtsSourceStatistics &statistics() const override {
    return field_.stats;
  }
  const format::FieldMetadata &metadata() const override {
    return field_.metadata;
  }
  uint64_t term_workspace_limit() const override {
    return 16 * 1024;
  }
  Result<FtsDocumentLength> document(uint32_t row) override {
    if (row >= data_->rows) {
      return tl::make_unexpected(
          Status::InvalidArgument("FTS row outside buffer"));
    }
    return field_.document(row);
  }
  Result<FtsSourceTerms::Ptr> terms() const override {
    return std::make_unique<ActiveTerms>(data_, field_);
  }
  Result<format::FieldMetadata> write_field(format::FieldMetadata metadata,
                                            FtsContainerWriter *container,
                                            uint32_t ordinal) const override {
    metadata.total_tokens = field_.stats.total_tokens;
    metadata.scoring_document_count = field_.stats.scoring_documents;
    metadata.present_count = field_.stats.present;
    auto writer = FtsFieldWriter::Create(data_->rows, std::move(metadata), {},
                                         container->workspace());
    if (!writer) {
      return tl::make_unexpected(writer.error());
    }
    for (uint32_t row = 0; row < data_->rows; ++row) {
      const auto doc = field_.document(row);
      auto status = (*writer)->add_document(row, doc.present, doc.length);
      CHECK_RETURN_STATUS_EXPECTED(status);
    }
    auto status = (*writer)->finish_documents();
    CHECK_RETURN_STATUS_EXPECTED(status);
    for (uint32_t i = 0; i < field_.terms; ++i) {
      const auto *entry = field_.ordered[i];
      if (entry->key.size() <= format::kDictionaryBlockBytes) {
        status = (*writer)->begin_term(entry->key);
      } else {
        status = (*writer)->begin_term(
            entry->key.size(),
            [key = entry->key](uint64_t at, size_t len) -> Result<std::string> {
              if (!format::ValidRange(at, len, key.size())) {
                return tl::make_unexpected(
                    Status::InternalError("Invalid active term slice"));
              }
              return std::string(key.substr(at, len));
            });
      }
      CHECK_RETURN_STATUS_EXPECTED(status);
      uint32_t df = 0;
      for (auto *posting = entry->head; posting; posting = posting->next) {
        PositionCursor positions(data_, posting);
        status = (*writer)->add_posting(posting->row, posting->tf,
                                        [&] { return positions.next(); });
        CHECK_RETURN_STATUS_EXPECTED(status);
        ++df;
      }
      if (df != entry->df) {
        return tl::make_unexpected(
            Status::InternalError("FTS active posting count mismatch"));
      }
      status = (*writer)->end_term();
      CHECK_RETURN_STATUS_EXPECTED(status);
    }
    return (*writer)->finish(container, ordinal);
  }

 private:
  std::shared_ptr<Data> data_;
  const Field &field_;
};

struct PendingPosting {
  Entry *entry;
  Posting *posting;
  Positions *last_positions;
  uint32_t collision;  // One-based pending posting index.
  bool new_term;
};

struct PendingField {
  FtsDocumentLength document;
  FtsDocumentLength *new_block{nullptr};
};

}  // namespace

struct PreparedFtsDocument::Impl {
  Impl(std::shared_ptr<Data> owner, FtsWriteBudget::Lease lease)
      : data(std::move(owner)),
        prepare_lease(std::move(lease)),
        mark(data->arena.used),
        sort_mark(data->arena.sort_bytes) {}
  ~Impl() {
    if (data) {
      observe_vectors();
      data->arena.used = mark;
      data->arena.sort_bytes = sort_mark;
      data->preparing = false;
      if (!data->rows) {
        data->table = nullptr;
        data->arena.release();
        data->arena_lease = {};
      }
    }
  }
  Status add(uint32_t field, std::string_view key, uint32_t position);
  void observe_vectors() const {
    data->budget->observe_prepare(
        fields.capacity() * sizeof(PendingField) +
            postings.capacity() * sizeof(PendingPosting) +
            buckets.capacity() * sizeof(uint32_t),
        data->arena.used);
  }

  std::shared_ptr<Data> data;
  FtsWriteBudget::Lease prepare_lease;
  size_t mark, sort_mark;
  std::vector<PendingField> fields;
  std::vector<PendingPosting> postings;
  std::vector<uint32_t> buckets;
};

Status PreparedFtsDocument::Impl::add(uint32_t field, std::string_view key,
                                      uint32_t position) {
  auto &arena = data->arena;
  const auto hash = Data::hash_key(field, key);
  if (postings.size() >= buckets.size()) {
    // Budget for simultaneous old/new vectors, allocator rounding and per-
    // field state. The other half belongs to the analyzer.
    size_t capacity = buckets.empty() ? 8 : buckets.size() * 2;
    if (capacity > UINT32_MAX ||
        capacity > (data->prepare_limit / 2 -
                    fields.capacity() * sizeof(PendingField)) /
                       (3 * (sizeof(PendingPosting) + sizeof(uint32_t)))) {
      return Full();
    }
    postings.reserve(capacity);
    buckets.assign(capacity, 0);
    for (size_t i = 0; i < postings.size(); ++i) {
      auto &p = postings[i];
      size_t slot = p.entry->hash & (capacity - 1);
      p.collision = buckets[slot];
      buckets[slot] = static_cast<uint32_t>(i + 1);
    }
  }
  auto &head = buckets[hash & (buckets.size() - 1)];
  uint32_t id = head;
  while (id) {
    const auto &p = postings[id - 1];
    if (p.entry->hash == hash && p.entry->field == field &&
        p.entry->key == key) {
      break;
    }
    id = p.collision;
  }
  if (!id) {
    auto *entry = data->find(field, key, hash);
    const bool is_new = entry == nullptr;
    if (is_new) {
      entry = arena.array<Entry>();
      auto *bytes = arena.array<char>(key.size());
      if (!entry || !bytes || !arena.reserve_key()) {
        return Full();
      }
      if (!key.empty()) {
        std::memcpy(bytes, key.data(), key.size());
      }
      entry->key = {bytes, key.size()};
      entry->field = field;
      entry->hash = hash;
    }
    auto *posting = arena.array<Posting>();
    if (!posting) {
      return Full();
    }
    posting->row = data->rows;
    postings.push_back({entry, posting, nullptr, head, is_new});
    id = head = static_cast<uint32_t>(postings.size());
  }
  auto &p = postings[id - 1];
  auto &length = fields[field].document.length;
  if (length == UINT32_MAX) {
    return Status::InvalidArgument("FTS field length exceeds uint32");
  }
  if (p.posting->tf == 0) {
    p.posting->first = position;
  } else {
    size_t slot = (p.posting->tf - 1) % Positions{}.values.size();
    if (slot == 0) {
      auto *block = arena.array<Positions>();
      if (!block) {
        return Full();
      }
      if (p.last_positions) {
        p.last_positions->next = block;
      } else {
        p.posting->extra = block;
      }
      p.last_positions = block;
    }
    p.last_positions->values[slot] = position;
  }
  ++p.posting->tf;
  ++length;
  return Status::OK();
}

struct FtsActiveBuffer::Impl {
  std::shared_ptr<Data> data;
};

PreparedFtsDocument::PreparedFtsDocument(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
PreparedFtsDocument::~PreparedFtsDocument() = default;
FtsActiveBuffer::FtsActiveBuffer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
FtsActiveBuffer::~FtsActiveBuffer() = default;

Result<std::shared_ptr<FtsActiveBuffer>> FtsActiveBuffer::Create(
    const FieldSchemaPtrList &fields, size_t active_bytes, size_t prepare_bytes,
    std::shared_ptr<FtsWriteBudget> budget) {
  if (fields.empty() || fields.size() > format::kMaxFields ||
      prepare_bytes / 2 < fields.size() * sizeof(PendingField) + 1024) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS active configuration"));
  }
  auto metadata = detail::FtsAnalysisMetadataBytes(
      fields, 1024 + fields.size() * (sizeof(Field) + 128), active_bytes);
  if (!metadata) {
    return tl::make_unexpected(metadata.error());
  }
  const size_t overhead = *metadata;
  if (active_bytes - overhead <= 1024) {
    return tl::make_unexpected(Full());
  }
  if (!budget) {
    budget = std::make_shared<FtsWriteBudget>(active_bytes, prepare_bytes);
  }
  if (active_bytes - overhead > budget->limit(FtsWriteBudget::Part::Active) ||
      prepare_bytes > budget->limit(FtsWriteBudget::Part::Prepare)) {
    return tl::make_unexpected(Status::InvalidArgument(
        "FTS writer limits exceed the shared collection budget"));
  }
  auto config = budget->reserve(FtsWriteBudget::Part::Metadata, overhead);
  if (!config) {
    return tl::make_unexpected(config.error());
  }
  auto data = std::make_shared<Data>(active_bytes - overhead, prepare_bytes,
                                     budget, std::move(*config));
  const auto target_buckets = static_cast<uint32_t>(std::max<size_t>(
      8, std::min<size_t>(1 << 20, (active_bytes - overhead) / 256)));
  data->buckets = size_t{1} << (31 - ailego_clz32(target_buckets));
  data->fields.reserve(fields.size());
  for (const auto &schema : fields) {
    auto params =
        std::static_pointer_cast<zvec::FtsIndexParams>(schema->index_params());
    auto pipeline = detail::AcquireFtsWritingPipeline(*params);
    if (!pipeline) {
      return tl::make_unexpected(pipeline.error());
    }
    auto status = (*pipeline)->validate_streaming(prepare_bytes / 2);
    if (!status.ok()) {
      return tl::make_unexpected(status);
    }
    auto &field = data->fields.emplace_back();
    field.pipeline = *pipeline;
    field.metadata.name = schema->name();
    ManifestCodec::EncodeIndexParams(params.get(),
                                     &field.metadata.analysis_identity);
  }
  auto impl = std::make_unique<Impl>();
  impl->data = std::move(data);
  return std::shared_ptr<FtsActiveBuffer>(new FtsActiveBuffer(std::move(impl)));
}

uint32_t FtsActiveBuffer::rows() const {
  return impl_->data->rows;
}

bool FtsActiveBuffer::preparing() const {
  return impl_->data->preparing;
}

bool FtsActiveBuffer::discard(PreparedFtsDocument &document) noexcept {
  if (!document.impl_ || document.impl_->data != impl_->data) {
    return false;
  }
  document.impl_.reset();
  return true;
}

Result<std::unique_ptr<PreparedFtsDocument>> FtsActiveBuffer::prepare(
    const std::vector<std::optional<std::string_view>> &fields) {
  auto &data = *impl_->data;
  if (data.preparing || data.frozen || data.rows == UINT32_MAX ||
      fields.size() != data.fields.size()) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS prepare state or fields"));
  }
  auto admission =
      data.budget->reserve(FtsWriteBudget::Part::Prepare, data.prepare_limit);
  if (!admission) {
    return tl::make_unexpected(admission.error());
  }
  auto status = data.start_arena();
  CHECK_RETURN_STATUS_EXPECTED(status);
  auto doc = std::make_unique<PreparedFtsDocument::Impl>(
      std::shared_ptr<Data>(shared_from_this(), impl_->data.get()),
      std::move(*admission));
  data.preparing = true;
  doc->fields.resize(fields.size());
  const RowLocation loc(data.rows);
  for (uint32_t i = 0; i < fields.size(); ++i) {
    auto &pending = doc->fields[i];
    if (loc.within == 0) {
      pending.new_block =
          data.arena.array<FtsDocumentLength>(size_t{1} << loc.block);
      if (!pending.new_block) {
        return tl::make_unexpected(Full());
      }
    }
    if (fields[i]) {
      pending.document.present = true;
      auto status = data.fields[i].pipeline->analyze_to(
          *fields[i],
          [&](std::string_view key, uint32_t, uint32_t pos) {
            return doc->add(i, key, pos);
          },
          data.prepare_limit / 2);
      if (!status.ok()) {
        return tl::make_unexpected(status);
      }
    }
  }
  return std::unique_ptr<PreparedFtsDocument>(
      new PreparedFtsDocument(std::move(doc)));
}

void FtsActiveBuffer::apply(PreparedFtsDocument &document) noexcept {
  if (!document.impl_) {
    return;
  }
  auto &doc = *document.impl_;
  if (doc.data != impl_->data) {
    return;
  }
  auto &data = *doc.data;
  RowLocation loc(data.rows);
  for (size_t i = 0; i < doc.fields.size(); ++i) {
    auto &field = data.fields[i];
    auto &pending = doc.fields[i];
    if (pending.new_block) {
      field.documents[loc.block] = pending.new_block;
    }
    field.documents[loc.block][loc.within] = pending.document;
    ++field.stats.rows;
    field.stats.present += pending.document.present;
    field.stats.scoring_documents += pending.document.present;
    field.stats.total_tokens += pending.document.length;
  }
  for (const auto &pending : doc.postings) {
    Entry *entry = pending.entry;
    if (pending.new_term) {
      auto &bucket = data.table[entry->hash & (data.buckets - 1)];
      entry->collision = bucket;
      bucket = entry;
      ++data.fields[entry->field].terms;
    }
    if (entry->tail) {
      entry->tail->next = pending.posting;
    } else {
      entry->head = pending.posting;
    }
    entry->tail = pending.posting;
    ++entry->df;
  }
  ++data.rows;
  data.preparing = false;
  doc.observe_vectors();
  doc.data.reset();
  document.impl_.reset();
}

Result<FtsSegmentReader::Ptr> FtsActiveBuffer::reader(uint32_t field) {
  if (field >= impl_->data->fields.size()) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS field ordinal"));
  }
  return std::make_shared<ActiveReader>(
      std::shared_ptr<Data>(shared_from_this(), impl_->data.get()), field);
}

Result<FtsFieldSource::Ptr> FtsActiveBuffer::source(uint32_t ordinal) {
  auto &data = *impl_->data;
  if (ordinal >= data.fields.size() || data.preparing) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS source state or field"));
  }
  if (!data.frozen) {
    auto status = data.arena_lease.freeze();
    CHECK_RETURN_STATUS_EXPECTED(status);
    // Each distinct key prepaid one pointer. Allocate a single aligned array
    // for all fields, then sort each field's slice in place.
    size_t count = data.arena.sort_bytes / sizeof(Entry *);
    data.arena.sort_bytes = 0;
    auto **sorted = count ? data.arena.array<Entry *>(count) : nullptr;
    if (count && !sorted) {
      data.arena.sort_bytes = count * sizeof(Entry *);
      return tl::make_unexpected(Full());
    }
    for (auto &field : data.fields) {
      field.ordered = sorted;
      if (field.terms) {
        sorted += field.terms;
      }
      field.metadata.term_count = field.terms;
      field.metadata.present_count = field.stats.present;
      field.metadata.scoring_document_count = field.stats.scoring_documents;
      field.metadata.total_tokens = field.stats.total_tokens;
      field.terms = 0;
    }
    for (size_t i = 0; count && i < data.buckets; ++i) {
      for (auto *entry = data.table[i]; entry; entry = entry->collision) {
        auto &field = data.fields[entry->field];
        field.ordered[field.terms++] = entry;
      }
    }
    for (auto &field : data.fields) {
      if (!field.terms) {
        continue;
      }
      std::sort(field.ordered, field.ordered + field.terms,
                [](const Entry *a, const Entry *b) { return a->key < b->key; });
    }
    data.frozen = true;
    data.budget->observe_frozen(data.arena.used);
  }
  return std::make_shared<ActiveSource>(
      std::shared_ptr<Data>(shared_from_this(), impl_->data.get()), ordinal);
}

}  // namespace zvec::fts
