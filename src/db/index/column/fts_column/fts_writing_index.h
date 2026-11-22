// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <future>
#include "fts_active_buffer.h"
#include "fts_indexer.h"
#include "fts_run.h"

namespace zvec::fts {

struct FtsWritingOptions {
  static FtsWritingOptions WithBudget(std::shared_ptr<FtsWriteBudget> budget) {
    FtsWritingOptions options;
    options.memory = std::move(budget);
    return options;
  }
  size_t active_bytes{64u << 20};
  size_t prepare_bytes{32u << 20};
  size_t soft_runs{8};
  size_t hard_runs{FtsWritingDescriptor::kMaxRuns};
  uint64_t document_spill_bytes{UINT64_C(1) << 30};
  uint64_t temporary_bytes_per_job{UINT64_C(4) << 30};
  uint64_t max_uncheckpointed_bytes{UINT64_C(4) << 30};
  uint64_t max_retired_bytes{UINT64_C(4) << 30};
  std::shared_ptr<FtsWriteBudget> memory;
};

// A single externally serialized parent writer. Background encoders own
// immutable inputs and never acquire the caller's locks. Only the foreground
// publishes their results. A reader is used under the collection read lock.
class FtsWritingIndex {
 public:
  using Ptr = std::unique_ptr<FtsWritingIndex>;
  using Fields = std::vector<std::optional<std::string_view>>;
  static Result<Ptr> Open(std::string directory, uint32_t segment,
                          uint32_t block, bool mmap,
                          const CollectionSchema &schema,
                          FtsRetirementQueue::Pointer retirement,
                          std::shared_ptr<FtsReadBudget> read_budget,
                          FtsWritingOptions options = {},
                          const FtsWritingDescriptor *checkpoint = nullptr);
  ~FtsWritingIndex();

  Result<std::unique_ptr<PreparedFtsDocument>> prepare(const Fields &values);
  void apply(PreparedFtsDocument &doc);
  Result<FtsColumnIndexerPtr> reader(const std::string &field) const;
  Status flush();
  Status import_legacy(const FtsIndexer &legacy,
                       const FtsIndexer::CustomTarget &target);
  Result<FtsIndexer::Ptr> seal(const FtsIndexer::CustomTarget &target,
                               const DurableFile::Hook &hook = {});
  Result<FtsWritingDescriptor> checkpoint(uint64_t epoch);
  void preserve_checkpoint();
  void publish_checkpoint();
  void retire_checkpoint();
  uint32_t rows() const;
  FtsWriteBudget::Usage memory_usage() const {
    return options_.memory->usage();
  }
  bool needs_checkpoint(uint64_t wal_bytes) const;
  size_t flush_count() const {
    return flush_count_;
  }
  size_t merge_count() const {
    return merge_count_;
  }
  // File footprints of accepted flushes (including direct one-row runs)
  // and merges in this session. Checkpoint inputs and legacy import are
  // excluded. Each merge input file counts once regardless of page reuse.
  struct IoStats {
    uint64_t flush_output_bytes{0};
    uint64_t merge_input_bytes{0}, merge_output_bytes{0};
    FtsSpoolStats flush_spools, merge_spools;
  };
  const IoStats &io_stats() const {
    return io_stats_;
  }
  uint64_t run_file_bytes() const;

 private:
  using Run = std::shared_ptr<FtsRun>;
  using Sources = std::vector<std::vector<FtsFieldSource::Ptr>>;
  using Job = std::future<Result<Run>>;
  FtsWritingIndex() = default;
  Status freeze();
  Status publish_flush();
  Status schedule_merge();
  Status publish_merge();
  Result<Sources> sources(size_t first, size_t count) const;
  Job encode(uint32_t begin, Sources sources, FtsWriteBudget::Part part);

  std::string directory_;
  uint32_t segment_{0}, block_{0}, active_begin_{0};
  bool mmap_{false};
  FieldSchemaPtrList fields_;
  FtsWritingOptions options_;
  FtsRetirementQueue::Pointer retirement_;
  std::shared_ptr<FtsReadBudget> read_budget_;
  format::Identity writer_id_{};
  format::Digest schema_id_{};
  std::shared_ptr<FtsActiveBuffer> active_, frozen_;
  std::weak_ptr<FtsActiveBuffer> retired_buffer_;
  std::vector<Run> runs_, committed_;
  size_t merge_first_{0}, merge_size_{0}, flush_count_{0}, merge_count_{0};
  IoStats io_stats_;
  // Workers capture only immutable inputs and run creation parameters.
  Job flush_job_, merge_job_;
};

}  // namespace zvec::fts
