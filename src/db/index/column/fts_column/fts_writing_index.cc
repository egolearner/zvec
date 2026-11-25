// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#include "fts_writing_index.h"
#include <algorithm>
#include <filesystem>
#include <zvec/ailego/utility/file_helper.h>
#include "db/common/typedef.h"
#include "db/index/common/manifest_codec.h"
#include "fts_document_spill.h"
#include "fts_writing_reader.h"

namespace zvec::fts {

namespace {

// Fresh runs must fit their admission quota. Rewriting immutable runs also
// needs space proportional to the data already accepted, including after
// reopen. Cap that allowance by free space on the candidate's filesystem.
Result<uint64_t> RewriteDiskBudget(const std::string &directory,
                                   const std::vector<FtsRun::Ptr> &runs,
                                   uint64_t job_bytes) {
  if (runs.empty()) {
    return job_bytes;
  }
  std::error_code error;
  auto available =
      std::filesystem::space(ailego::FileHelper::PathFromUtf8(directory), error)
          .available;
  if (error) {
    return tl::make_unexpected(Status::InternalError(
        "Cannot measure FTS rewrite space: ", error.message()));
  }
  uint64_t limit = std::min<uint64_t>(available, job_bytes);
  auto allow = [&](uint64_t count, uint64_t bytes) {
    const uint64_t remaining = available - limit;
    limit += count > remaining / bytes ? remaining : count * bytes;
  };
  for (const auto &run : runs) {
    const auto &meta = run->container()->metadata();
    // Input streams, their output copies, and checksum/framing overhead.
    allow(run->descriptor().file_size, 4);
    for (const auto &field : meta.fields) {
      // Raw document rows, posting staging, position indexes and encoded
      // blocks. A one-token posting is the worst case for fixed framing.
      allow(meta.row_count, 32);
      allow(field.total_tokens, 512);
    }
    for (const auto &section : meta.sections) {
      // A prefix-coded dictionary block can expand to at most one copy of
      // its full first key per entry. Charge both spool and container copies.
      constexpr std::string_view suffix = ".terms";
      if (section.name.size() >= suffix.size() &&
          section.name.compare(section.name.size() - suffix.size(),
                               suffix.size(), suffix) == 0) {
        allow(section.length, 2 * format::kDictionaryBlockTerms);
      }
    }
  }
  return limit;
}

}  // namespace

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
                                        options.prepare_bytes, options.memory,
                                        read_budget);
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

Status FtsWritingIndex::encode(uint32_t begin, FtsRun::OpenField open_field,
                               FtsWriteBudget::Part part, Job *job,
                               uint64_t temporary_bytes) {
  auto workspace = options_.memory->reserve_job(part);
  if (!workspace) {
    return workspace.error();
  }
  try {
    *job = std::async(
        std::launch::async,
        [dir = directory_, mmap = mmap_, segment = segment_, block = block_,
         begin, count = fields_.size(), open_field = std::move(open_field),
         retirement = retirement_, budget = read_budget_, writer = writer_id_,
         temporary_bytes, workspace = std::move(*workspace)]() mutable {
          // The future can outlive execution. Release the slot on worker exit.
          auto lease = std::move(workspace);
          return FtsRun::Write(dir, mmap, segment, block, begin, count,
                               open_field, retirement, budget, writer,
                               temporary_bytes);
        });
  } catch (const std::system_error &error) {
    return {StatusCode::RESOURCE_EXHAUSTED,
            std::string("Cannot start FTS encoder: ") + error.what()};
  }
  return Status::OK();
}

Status FtsWritingIndex::start_flush() {
  return encode(
      active_begin_ - frozen_->rows(),
      [input = frozen_](uint32_t field) -> Result<FtsRun::FieldSources> {
        auto source = input->source(field);
        if (!source) {
          return tl::make_unexpected(source.error());
        }
        return FtsRun::FieldSources{std::move(*source)};
      },
      FtsWriteBudget::Part::Flush, &flush_job_,
      options_.temporary_bytes_per_job);
}

Status FtsWritingIndex::publish_flush() {
  if (!frozen_) {
    return Status::OK();
  }
  // On an I/O or scheduling failure the frozen input remains retryable.
  if (!flush_job_.valid()) {
    auto status = start_flush();
    CHECK_RETURN_STATUS(status);
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
                                      options_.prepare_bytes, options_.memory,
                                      read_budget_);
  if (!next) {
    return next.error();
  }
  // Complete the in-place sort and transfer its memory lease on the writer
  // thread before readers or the worker can observe the frozen buffer.
  auto first_field = active_->source(0);
  if (!first_field) {
    return first_field.error();
  }
  frozen_ = std::move(active_);
  active_ = std::move(*next);
  active_begin_ += frozen_->rows();
  return start_flush();
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
  auto first = runs_.begin() + merge_first_;
  std::vector<Run> inputs(first, first + merge_size_);
  const auto begin = inputs.front()->begin();
  auto disk_budget =
      RewriteDiskBudget(directory_, inputs, options_.temporary_bytes_per_job);
  if (!disk_budget) {
    return disk_budget.error();
  }
  return encode(
      begin,
      [inputs =
           std::move(inputs)](uint32_t field) -> Result<FtsRun::FieldSources> {
        FtsRun::FieldSources sources;
        for (const auto &run : inputs) {
          auto source = run->source(field);
          if (!source) {
            return tl::make_unexpected(source.error());
          }
          sources.push_back(std::move(*source));
        }
        return sources;
      },
      FtsWriteBudget::Part::Merge, &merge_job_, *disk_budget);
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
  auto combined = OpenWritingFtsReader(std::move(readers), std::move(domains),
                                       read_budget_);
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
  auto imported = FtsRun::Write(
      directory_, mmap_, segment_, block_, 0, fields_.size(),
      [&](uint32_t field) -> Result<FtsRun::FieldSources> {
        auto source = (*snapshot)->open_source(fields_[field]->name(),
                                               target.block.doc_count());
        if (!source) {
          return tl::make_unexpected(source.error());
        }
        return FtsRun::FieldSources{std::move(*source)};
      },
      retirement_, read_budget_, writer_id_, options_.temporary_bytes_per_job);
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
  auto disk_budget =
      RewriteDiskBudget(directory_, runs_, options_.temporary_bytes_per_job);
  if (!disk_budget) {
    return tl::make_unexpected(disk_budget.error());
  }
  if (runs_.size() == 1) {
    return FtsIndexer::PrepareRunSeal(target, fields_, *runs_.front(),
                                      writer_id_, retirement_, hook,
                                      *disk_budget);
  }
  auto workspace = options_.memory->reserve_job(FtsWriteBudget::Part::Merge);
  if (!workspace) {
    return tl::make_unexpected(workspace.error());
  }
  std::shared_ptr<FtsActiveBuffer> empty;
  if (runs_.empty()) {
    auto buffer = FtsActiveBuffer::Create(fields_, options_.active_bytes,
                                          options_.prepare_bytes,
                                          options_.memory, read_budget_);
    if (!buffer) {
      return tl::make_unexpected(buffer.error());
    }
    empty = std::move(*buffer);
  }
  auto open_source = [&](size_t run, const FieldSchema::Ptr &field) {
    const auto ordinal =
        std::find(fields_.begin(), fields_.end(), field) - fields_.begin();
    return empty ? empty->source(ordinal) : runs_[run]->source(ordinal);
  };
  return FtsIndexer::PrepareMerged(
      target, fields_, std::max<size_t>(1, runs_.size()), open_source, {},
      FtsMergePolicy::kCoalesceRuns, retirement_, hook, *disk_budget);
}

}  // namespace zvec::fts
