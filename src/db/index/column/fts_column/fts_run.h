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

#include "db/index/common/meta.h"
#include "fts_file_lifetime.h"
#include "fts_merge.h"
#include "fts_stream.h"

namespace zvec::fts {

format::Identity NewFtsIdentity();

// Immutable, private until its enclosing collection version commits. Its
// readers expose local row IDs only to the parent reader and merge code.
class FtsRun {
 public:
  using Ptr = std::shared_ptr<FtsRun>;

  using FieldSources = std::vector<FtsFieldSource::Ptr>;
  using OpenField = std::function<Result<FieldSources>(uint32_t)>;

  static Result<Ptr> Write(
      const std::string &segment_path, bool mmap, uint64_t segment_id,
      uint32_t block_id, uint32_t parent_begin,
      const std::vector<std::vector<FtsFieldSource::Ptr>> &sources,
      const FtsRetirementQueue::Pointer &retirement,
      const std::shared_ptr<FtsReadBudget> &read_budget,
      const format::Identity &writer_id = {},
      uint64_t temporary_bytes = UINT64_C(4) << 30);

  static Result<Ptr> Write(const std::string &segment_path, bool mmap,
                           uint64_t segment_id, uint32_t block_id,
                           uint32_t parent_begin, size_t field_count,
                           const OpenField &open_field,
                           const FtsRetirementQueue::Pointer &retirement,
                           const std::shared_ptr<FtsReadBudget> &read_budget,
                           const format::Identity &writer_id = {},
                           uint64_t temporary_bytes = UINT64_C(4) << 30);

  static Result<Ptr> Open(const std::string &segment_path, bool mmap,
                          uint64_t segment_id, uint32_t block_id,
                          const FtsRunDescriptor &descriptor,
                          const FtsRetirementQueue::Pointer &retirement,
                          const std::shared_ptr<FtsReadBudget> &read_budget);

  Result<FtsSegmentReader::Ptr> reader(uint32_t field) const;
  Result<FtsFieldSource::Ptr> source(uint32_t field) const;
  static bool SourceMatches(const FtsFieldSource &source, uint64_t segment_id,
                            uint32_t block_id, uint64_t begin, uint32_t field);
  bool matches_field(uint32_t field, const std::string &name,
                     std::string_view analysis_identity,
                     const format::Identity &writer_id) const;
  uint32_t begin() const {
    return descriptor_.parent_begin;
  }
  uint32_t rows() const {
    return descriptor_.rows;
  }
  const FtsRunDescriptor &descriptor() const {
    return descriptor_;
  }
  const FtsSpoolStats &spool_stats() const {
    return spool_stats_;
  }
  const std::string &path() const {
    return path_;
  }
  const FtsFileOwner::Pointer &owner() const {
    return owner_;
  }
  const FtsContainerReader::Pointer &container() const {
    return container_;
  }

 private:
  static Result<Ptr> OpenFile(const std::string &path,
                              FtsContainerReader::Pointer container,
                              uint64_t segment_id, uint32_t block_id,
                              const FtsRunDescriptor &descriptor,
                              FtsFileOwner::Pointer owner);
  std::string path_;
  FtsRunDescriptor descriptor_;
  FtsSpoolStats spool_stats_;
  FtsFileOwner::Pointer owner_;
  FtsContainerReader::Pointer container_;
  std::vector<FtsFieldReader::Pointer> fields_;
};

}  // namespace zvec::fts
