// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <zvec/db/schema.h>
#include "fts_source.h"
#include "fts_write_budget.h"

namespace zvec::fts {

class FtsActiveBuffer;
class FtsRun;

// Frozen memory already owns its sorted keys and posting chains. Encoding
// directly avoids allocating query cursors for every posting in a flush.
class FtsFrozenFieldSource : public FtsFieldSource {
 public:
  virtual Result<format::FieldMetadata> write_field(
      format::FieldMetadata metadata, FtsContainerWriter *container,
      uint32_t ordinal) const = 0;
};

// An outstanding document pins its buffer and rolls back its private arena
// reservation on destruction. Only one reservation can exist per buffer.
class PreparedFtsDocument {
 public:
  ~PreparedFtsDocument();
  PreparedFtsDocument(const PreparedFtsDocument &) = delete;
  PreparedFtsDocument &operator=(const PreparedFtsDocument &) = delete;

 private:
  friend class FtsActiveBuffer;
  friend class FtsWritingIndex;
  struct Impl;
  explicit PreparedFtsDocument(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  std::shared_ptr<FtsRun> spilled_run_;
};

class FtsActiveBuffer : public std::enable_shared_from_this<FtsActiveBuffer> {
 public:
  static Result<std::shared_ptr<FtsActiveBuffer>> Create(
      const FieldSchemaPtrList &fields, size_t active_bytes,
      size_t prepare_bytes, std::shared_ptr<FtsWriteBudget> budget = {});
  ~FtsActiveBuffer();
  Result<std::unique_ptr<PreparedFtsDocument>> prepare(
      const std::vector<std::optional<std::string_view>> &fields);
  void apply(PreparedFtsDocument &document) noexcept;
  uint32_t rows() const;
  bool preparing() const;
  Result<FtsSegmentReader::Ptr> reader(uint32_t field);
  // Capturing a source freezes this buffer, permitting sorted enumeration.
  Result<FtsFieldSource::Ptr> source(uint32_t field);

 private:
  friend class PreparedFtsDocument;
  friend class FtsWritingIndex;
  bool discard(PreparedFtsDocument &document) noexcept;
  struct Impl;
  explicit FtsActiveBuffer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace zvec::fts
