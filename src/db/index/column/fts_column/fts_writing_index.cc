// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#include "fts_writing_index.h"
#include <algorithm>
#include "db/common/typedef.h"
#include "db/index/common/manifest_codec.h"
#include "fts_document_spill.h"
#include "fts_writing_reader.h"

namespace zvec::fts {

Result<FtsWritingIndex::Ptr> FtsWritingIndex::Open(
    std::string directory, uint32_t segment, uint32_t block, bool mmap,
    const CollectionSchema &schema, FtsRetirementQueue::Pointer retirement,
    std::shared_ptr<FtsReadBudget> read_budget, FtsWritingOptions options,
    const FtsWritingDescriptor *checkpoint) {
  auto fields = schema.fts_fields();
  if (fields.empty() || fields.size() > format::kMaxFields || !retirement ||
      !read_budget || options.soft_runs < 2 ||
      options.soft_runs > options.hard_runs ||
      options.hard_runs > FtsWritingDescriptor::kMaxRuns ||
      !options.max_uncheckpointed_bytes || !options.max_retired_bytes ||
      options.temporary_bytes_per_job < (1u << 16)) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS writing limits"));
  }
  if (!options.memory) {
    options.memory = std::make_shared<FtsWriteBudget>(options.active_bytes,
                                                      options.prepare_bytes);
  }
  auto active = FtsActiveBuffer::Create(fields, options.active_bytes,
                                        options.prepare_bytes, options.memory);
  if (!active) {
    return tl::make_unexpected(active.error());
  }
  Ptr index(new FtsWritingIndex);
  index->directory_ = std::move(directory);
  index->segment_ = segment;
  index->block_ = block;
  index->mmap_ = mmap;
  index->fields_ = std::move(fields);
  index->options_ = options;
  index->retirement_ = std::move(retirement);
  index->read_budget_ = std::move(read_budget);
  index->active_ = std::move(*active);
  index->runs_.reserve(options.hard_runs);
  index->committed_.reserve(options.hard_runs);
  index->schema_id_ = ManifestCodec::SchemaIdentity(schema);
  index->writer_id_ = checkpoint ? checkpoint->writer_id : NewFtsIdentity();
  if (checkpoint) {
    if (checkpoint->schema_identity != index->schema_id_ ||
        checkpoint->runs.size() > options.hard_runs ||
        checkpoint->writer_id == format::Identity{} ||
        checkpoint->wal_epoch == 0) {
      return tl::make_unexpected(
          Status::InvalidArgument("Invalid writing checkpoint"));
    }
    for (const auto &entry : checkpoint->runs) {
      if (entry.parent_begin != index->active_begin_ || entry.rows == 0 ||
          !format::ValidRange(entry.parent_begin, entry.rows, UINT32_MAX)) {
        return tl::make_unexpected(
            Status::InvalidArgument("Invalid FTS run coverage"));
      }
      auto run = FtsRun::Open(index->directory_, mmap, segment, block, entry,
                              index->retirement_, index->read_budget_);
      if (!run) {
        return tl::make_unexpected(run.error());
      }
      const auto &metadata = (*run)->container()->metadata().fields;
      if (metadata.size() != index->fields_.size()) {
        return tl::make_unexpected(
            Status::InvalidArgument("FTS run field count mismatch"));
      }
      for (size_t i = 0; i < metadata.size(); ++i) {
        std::string identity;
        ManifestCodec::EncodeIndexParams(
            index->fields_[i]->index_params().get(), &identity);
        if (!(*run)->matches_field(i, index->fields_[i]->name(), identity,
                                   index->writer_id_)) {
          return tl::make_unexpected(
              Status::InvalidArgument("FTS run field identity mismatch"));
        }
      }
      index->runs_.push_back(std::move(*run));
      index->active_begin_ += entry.rows;
    }
    if (index->active_begin_ != checkpoint->row_frontier) {
      return tl::make_unexpected(
          Status::InvalidArgument("Invalid FTS row frontier"));
    }
    index->committed_ = index->runs_;
  }
  return index;
}

FtsWritingIndex::~FtsWritingIndex() = default;

uint32_t FtsWritingIndex::rows() const {
  return active_begin_ + active_->rows();
}

uint64_t FtsWritingIndex::run_file_bytes() const {
  uint64_t bytes = 0;
  for (const auto &run : runs_) {
    bytes += run->descriptor().file_size;
  }
  return bytes;
}

bool FtsWritingIndex::needs_checkpoint(uint64_t wal_bytes) const {
  return wal_bytes >= options_.max_uncheckpointed_bytes ||
         retirement_->uncheckpointed_bytes() >=
             options_.max_uncheckpointed_bytes - wal_bytes;
}

Result<std::unique_ptr<PreparedFtsDocument>> FtsWritingIndex::prepare(
    const Fields &values) {
  if (retirement_->retired_bytes() >= options_.max_retired_bytes) {
    retirement_->retry();
    if (retirement_->retired_bytes() >= options_.max_retired_bytes) {
      return tl::make_unexpected(
          Status{StatusCode::RESOURCE_EXHAUSTED,
                 "FTS retired files exceed the disk budget"});
    }
  }
  if (rows() == UINT32_MAX) {
    return tl::make_unexpected(
        Status{StatusCode::RESOURCE_EXHAUSTED, "FTS parent row domain full"});
  }
  auto prepared = active_->prepare(values);
  if (prepared || prepared.error().code() != StatusCode::RESOURCE_EXHAUSTED) {
    return prepared;
  }
  if (active_->rows()) {
    auto status = freeze();
    CHECK_RETURN_STATUS_EXPECTED(status);
    prepared = active_->prepare(values);
    if (prepared || prepared.error().code() != StatusCode::RESOURCE_EXHAUSTED) {
      return prepared;
    }
  }
  // Direct one-row runs follow any older frozen range. Reserve the root slot,
  // finish encoding and open its reader before WAL. On cancellation the
  // candidate's last owner removes it and the active row remains available.
  auto status = publish_flush();
  CHECK_RETURN_STATUS_EXPECTED(status);
  status = schedule_merge();
  CHECK_RETURN_STATUS_EXPECTED(status);
  if (runs_.size() >= options_.hard_runs) {
    status = publish_merge();
    CHECK_RETURN_STATUS_EXPECTED(status);
  }
  auto reservation = active_->prepare(Fields(fields_.size()));
  if (!reservation) {
    return reservation;
  }
  auto workspace = options_.memory->reserve_job(FtsWriteBudget::Part::Flush);
  if (!workspace) {
    return tl::make_unexpected(workspace.error());
  }
  auto inputs = PrepareSpilledFtsDocument(fields_, values, directory_, block_,
                                          retirement_, options_.prepare_bytes,
                                          options_.document_spill_bytes);
  if (!inputs) {
    return tl::make_unexpected(inputs.error());
  }
  auto run = FtsRun::Write(directory_, mmap_, segment_, block_, active_begin_,
                           *inputs, retirement_, read_budget_, writer_id_,
                           options_.temporary_bytes_per_job);
  if (!run) {
    return tl::make_unexpected(run.error());
  }
  (*reservation)->spilled_run_ = std::move(*run);
  return reservation;
}

void FtsWritingIndex::apply(PreparedFtsDocument &doc) {
  if (doc.spilled_run_) {
    if (active_->discard(doc)) {
      io_stats_.flush_output_bytes += doc.spilled_run_->descriptor().file_size;
      io_stats_.flush_spools.add(doc.spilled_run_->spool_stats());
      runs_.push_back(std::move(doc.spilled_run_));
      ++active_begin_;
    }
  } else {
    active_->apply(doc);
  }
}

FtsWritingIndex::Job FtsWritingIndex::encode(uint32_t begin, Sources sources,
                                             FtsWriteBudget::Part part) {
  auto workspace = options_.memory->reserve_job(part);
  if (!workspace) {
    std::promise<Result<Run>> denied;
    denied.set_value(tl::make_unexpected(workspace.error()));
    return denied.get_future();
  }
  return std::async(
      std::launch::async,
      [dir = directory_, mmap = mmap_, segment = segment_, block = block_,
       begin, sources = std::move(sources), retirement = retirement_,
       budget = read_budget_, writer = writer_id_,
       temporary_bytes = options_.temporary_bytes_per_job,
       workspace = std::move(*workspace)]() mutable {
        // The future can outlive execution. Release the slot on worker exit.
        auto lease = std::move(workspace);
        return FtsRun::Write(dir, mmap, segment, block, begin, sources,
                             retirement, budget, writer, temporary_bytes);
      });
}

Status FtsWritingIndex::publish_flush() {
  if (!frozen_) {
    return Status::OK();
  }
  // On an I/O failure the complete frozen input remains visible and retryable.
  if (!flush_job_.valid()) {
    Sources inputs(fields_.size());
    for (size_t i = 0; i < fields_.size(); ++i) {
      auto source = frozen_->source(i);
      if (!source) {
        return source.error();
      }
      inputs[i].push_back(std::move(*source));
    }
    flush_job_ = encode(active_begin_ - frozen_->rows(), std::move(inputs),
                        FtsWriteBudget::Part::Flush);
  }
  auto run = flush_job_.get();
  if (!run) {
    return run.error();
  }
  io_stats_.flush_output_bytes += (*run)->descriptor().file_size;
  io_stats_.flush_spools.add((*run)->spool_stats());
  runs_.push_back(std::move(*run));
  retired_buffer_ = frozen_;
  frozen_.reset();
  ++flush_count_;
  return Status::OK();
}

Status FtsWritingIndex::freeze() {
  if (active_->preparing()) {
    return Status::FailedPrecondition(
        "Cannot flush an outstanding FTS document");
  }
  auto status = publish_flush();
  CHECK_RETURN_STATUS(status);
  if (!active_->rows()) {
    return Status::OK();
  }
  if (!retired_buffer_.expired()) {
    return {StatusCode::RESOURCE_EXHAUSTED,
            "An FTS reader still pins the frozen buffer"};
  }
  status = schedule_merge();
  CHECK_RETURN_STATUS(status);
  if (runs_.size() == options_.hard_runs) {
    status = publish_merge();
    CHECK_RETURN_STATUS(status);
  }
  auto next = FtsActiveBuffer::Create(fields_, options_.active_bytes,
                                      options_.prepare_bytes, options_.memory);
  if (!next) {
    return next.error();
  }
  Sources inputs(fields_.size());
  for (size_t i = 0; i < fields_.size(); ++i) {
    auto source = active_->source(i);
    if (!source) {
      return source.error();
    }
    inputs[i].push_back(std::move(*source));
  }
  frozen_ = std::move(active_);
  active_ = std::move(*next);
  const auto begin = active_begin_;
  active_begin_ += frozen_->rows();
  flush_job_ = encode(begin, std::move(inputs), FtsWriteBudget::Part::Flush);
  return Status::OK();
}

Result<FtsWritingIndex::Sources> FtsWritingIndex::sources(size_t first,
                                                          size_t count) const {
  Sources inputs(fields_.size());
  for (size_t i = 0; i < fields_.size(); ++i) {
    for (size_t j = first; j < first + count; ++j) {
      auto source = runs_[j]->source(i);
      if (!source) {
        return tl::make_unexpected(source.error());
      }
      inputs[i].push_back(std::move(*source));
    }
  }
  return inputs;
}

Status FtsWritingIndex::schedule_merge() {
  if (merge_job_.valid()) {
    if (merge_job_.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      return Status::OK();
    }
    auto status = publish_merge();
    CHECK_RETURN_STATUS(status);
  }
  if (runs_.size() < options_.soft_runs) {
    return Status::OK();
  }
  merge_size_ = std::min<size_t>(4, runs_.size());
  uint64_t smallest = UINT64_MAX;
  for (size_t i = 0; i + merge_size_ <= runs_.size(); ++i) {
    uint64_t bytes = 0;
    for (size_t j = i; j < i + merge_size_; ++j) {
      for (const auto &section : runs_[j]->container()->metadata().sections) {
        bytes += section.length;
      }
    }
    if (bytes < smallest) {
      smallest = bytes;
      merge_first_ = i;
    }
  }
  auto inputs = sources(merge_first_, merge_size_);
  if (!inputs) {
    return inputs.error();
  }
  merge_job_ = encode(runs_[merge_first_]->begin(), std::move(*inputs),
                      FtsWriteBudget::Part::Merge);
  return Status::OK();
}

Status FtsWritingIndex::publish_merge() {
  if (!merge_job_.valid()) {
    return Status::OK();
  }
  auto merged = merge_job_.get();
  if (!merged) {
    return merged.error();
  }
  auto first = runs_.begin() + merge_first_;
  for (auto it = first; it != first + merge_size_; ++it) {
    io_stats_.merge_input_bytes += (*it)->descriptor().file_size;
    if (std::find(committed_.begin(), committed_.end(), *it) ==
        committed_.end()) {
      (*it)->owner()->retire();
    }
  }
  io_stats_.merge_output_bytes += (*merged)->descriptor().file_size;
  io_stats_.merge_spools.add((*merged)->spool_stats());
  *first = std::move(*merged);
  runs_.erase(first + 1, first + merge_size_);
  ++merge_count_;
  retirement_->retry();
  return Status::OK();
}

Status FtsWritingIndex::flush() {
  auto status = freeze();
  CHECK_RETURN_STATUS(status);
  status = publish_flush();
  CHECK_RETURN_STATUS(status);
  return publish_merge();
}

Result<FtsColumnIndexerPtr> FtsWritingIndex::reader(
    const std::string &field) const {
  const auto found =
      std::find_if(fields_.begin(), fields_.end(),
                   [&](const auto &f) { return f->name() == field; });
  if (found == fields_.end()) {
    return tl::make_unexpected(Status::NotFound("Missing FTS field ", field));
  }
  auto ordinal = static_cast<uint32_t>(found - fields_.begin());
  std::vector<FtsSegmentReader::Ptr> readers;
  std::vector<uint32_t> domains;
  auto add = [&](const auto &input) {
    auto result = input->reader(ordinal);
    if (!result) {
      return result.error();
    }
    readers.push_back(std::move(*result));
    domains.push_back(input->rows());
    return Status::OK();
  };
  for (const auto &run : runs_) {
    auto status = add(run);
    CHECK_RETURN_STATUS_EXPECTED(status);
  }
  if (frozen_) {
    auto status = add(frozen_);
    CHECK_RETURN_STATUS_EXPECTED(status);
  }
  if (active_->rows() || readers.empty()) {
    auto status = add(active_);
    CHECK_RETURN_STATUS_EXPECTED(status);
  }
  auto combined = OpenWritingFtsReader(std::move(readers), std::move(domains));
  if (!combined) {
    return tl::make_unexpected(combined.error());
  }
  auto column = std::make_shared<FtsColumnIndexer>();
  auto opened = column->open_reader(field, std::move(*combined));
  if (!opened) {
    return tl::make_unexpected(opened.error());
  }
  return column;
}

Result<FtsWritingDescriptor> FtsWritingIndex::checkpoint(uint64_t epoch) {
  if (epoch == 0) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS WAL epoch"));
  }
  auto status = flush();
  CHECK_RETURN_STATUS_EXPECTED(status);
  FtsWritingDescriptor root;
  root.writer_id = writer_id_;
  root.schema_identity = schema_id_;
  root.wal_epoch = epoch;
  root.row_frontier = rows();
  for (const auto &run : runs_) {
    root.runs.push_back(run->descriptor());
  }
  return root;
}

Status FtsWritingIndex::import_legacy(const FtsIndexer &legacy,
                                      const FtsIndexer::CustomTarget &target) {
  if (rows() || frozen_ || !runs_.empty() || target.segment_id != segment_ ||
      target.block.id() != block_) {
    return Status::FailedPrecondition(
        "FTS import requires a fresh writing root");
  }
  if (!target.block.doc_count()) {
    return Status::OK();
  }
  // The mutable snapshot adapter recovers TF, positions, presence and exact
  // document lengths from the legacy CFs, without reanalyzing forward text.
  auto workspace = options_.memory->reserve_job(FtsWriteBudget::Part::Flush);
  if (!workspace) {
    return workspace.error();
  }
  auto snapshot = legacy.prepare_custom(target, fields_, retirement_);
  if (!snapshot) {
    return snapshot.error();
  }
  Sources inputs(fields_.size());
  for (size_t i = 0; i < fields_.size(); ++i) {
    auto source =
        (*snapshot)->open_source(fields_[i]->name(), target.block.doc_count());
    if (!source) {
      return source.error();
    }
    inputs[i].push_back(std::move(*source));
  }
  auto imported =
      FtsRun::Write(directory_, mmap_, segment_, block_, 0, inputs, retirement_,
                    read_budget_, writer_id_, options_.temporary_bytes_per_job);
  if (!imported) {
    return imported.error();
  }
  runs_.push_back(std::move(*imported));
  active_begin_ = target.block.doc_count();
  return Status::OK();
}

void FtsWritingIndex::preserve_checkpoint() {
  for (const auto &run : runs_) {
    run->owner()->preserve();
  }
}

void FtsWritingIndex::retire_checkpoint() {
  for (const auto &run : committed_) {
    run->owner()->retire();
  }
}

void FtsWritingIndex::publish_checkpoint() {
  preserve_checkpoint();
  for (const auto &old : committed_) {
    if (std::find(runs_.begin(), runs_.end(), old) == runs_.end()) {
      old->owner()->retire();
    }
  }
  committed_ = runs_;
}

Result<FtsIndexer::Ptr> FtsWritingIndex::seal(
    const FtsIndexer::CustomTarget &target, const DurableFile::Hook &hook) {
  auto status = flush();
  CHECK_RETURN_STATUS_EXPECTED(status);
  if (target.block.doc_count() != rows()) {
    return tl::make_unexpected(
        Status::InvalidArgument("FTS seal row count mismatch"));
  }
  if (runs_.size() == 1) {
    return FtsIndexer::PrepareRunSeal(target, fields_, *runs_.front(),
                                      writer_id_, retirement_, hook,
                                      options_.temporary_bytes_per_job);
  }
  auto workspace = options_.memory->reserve_job(FtsWriteBudget::Part::Merge);
  if (!workspace) {
    return tl::make_unexpected(workspace.error());
  }
  auto open_source = [&](size_t run, const FieldSchema::Ptr &field) {
    const auto ordinal = std::find(fields_.begin(), fields_.end(), field) - fields_.begin();
    return runs_.empty() ? active_->source(ordinal) : runs_[run]->source(ordinal);
  };
  return FtsIndexer::PrepareMerged(target, fields_, std::max<size_t>(1, runs_.size()),
                                   open_source, {},
                                   FtsMergePolicy::kCoalesceRuns, retirement_,
                                   hook, options_.temporary_bytes_per_job);
}

}  // namespace zvec::fts
