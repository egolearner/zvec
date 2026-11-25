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

#include "fts_run.h"
#include <algorithm>
#include <filesystem>
#include <rocksdb/env.h>
#include <zvec/core/framework/index_factory.h>
#include "db/common/durable_file.h"
#include "db/common/file_helper.h"
#include "fts_active_buffer.h"

namespace zvec::fts {
namespace {

format::Identity IdentityPrefix(const format::Digest &digest) {
  format::Identity id;
  std::copy_n(digest.begin(), id.size(), id.begin());
  return id;
}

format::Identity FieldIdentity(const format::Identity &writer_id,
                               std::string_view purpose, uint32_t ordinal) {
  format::Sha256 hash;
  hash.update(std::string_view(reinterpret_cast<const char *>(writer_id.data()),
                               writer_id.size()));
  hash.update(purpose);
  std::string bytes;
  format::AppendU32(ordinal, &bytes);
  hash.update(bytes);
  return IdentityPrefix(hash.digest());
}

// Adds physical provenance to the local field source. Coalescing can reject
// a reordered or foreign run before opening a posting cursor for the field.
class RunSource final : public FtsFieldSource {
 public:
  RunSource(FtsFieldSource::Ptr source, FtsContainerReader::Pointer container,
            uint32_t ordinal)
      : source_(std::move(source)),
        container_(std::move(container)),
        ordinal_(ordinal) {}
  const FtsSourceStatistics &statistics() const override {
    return source_->statistics();
  }
  const format::FieldMetadata &metadata() const override {
    return source_->metadata();
  }
  uint64_t term_workspace_limit() const override {
    return source_->term_workspace_limit();
  }
  Result<FtsDocumentLength> document(uint32_t row) override {
    return source_->document(row);
  }
  Result<FtsSourceTerms::Ptr> terms() const override {
    auto view = container_->scan_view();
    if (view == container_) {
      return source_->terms();
    }
    auto field = FtsFieldReader::Open(std::move(view), ordinal_);
    if (!field) {
      return tl::make_unexpected(field.error());
    }
    auto scan = OpenCustomFtsFieldSource(std::move(*field));
    if (!scan) {
      return tl::make_unexpected(scan.error());
    }
    return (*scan)->terms();
  }
  bool matches(uint64_t segment, uint32_t block, uint64_t begin,
               uint32_t ordinal) const {
    const auto &metadata = container_->metadata();
    return metadata.source_segment_id == segment &&
           metadata.block_id == block && metadata.parent_row_begin == begin &&
           ordinal_ == ordinal;
  }

 private:
  FtsFieldSource::Ptr source_;
  FtsContainerReader::Pointer container_;
  uint32_t ordinal_;
};

Result<uint32_t> ValidateField(uint64_t segment, uint32_t block, uint32_t begin,
                               uint32_t f, const FtsRun::FieldSources &sources,
                               const format::Identity &writer_id,
                               std::vector<uint32_t> *segment_rows) {
  if (sources.empty() || sources.size() > 4 ||
      (f != 0 && sources.size() != segment_rows->size())) {
    return tl::make_unexpected(Status::InvalidArgument("Invalid run fan-in"));
  }
  segment_rows->resize(sources.size());
  uint64_t end = begin;
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto &source = sources[i];
    if (!source || source->statistics().rows == 0 ||
        (f != 0 && source->statistics().rows != (*segment_rows)[i]) ||
        !format::ValidRange(end, source->statistics().rows, UINT32_MAX)) {
      return tl::make_unexpected(
          Status::InvalidArgument("Invalid run row domain"));
    }
    const auto *run = dynamic_cast<const RunSource *>(source.get());
    const auto &first = sources.front()->metadata();
    const auto &field = source->metadata();
    if (run && i == 0 && writer_id != format::Identity{} &&
        (field.id != FieldIdentity(writer_id, "field", f) ||
         field.incarnation != FieldIdentity(writer_id, "incarnation", f))) {
      return tl::make_unexpected(
          Status::InvalidArgument("Run does not belong to this writer"));
    }
    if ((!run && sources.size() != 1) ||
        (run && !run->matches(segment, block, end, f)) ||
        field.name != first.name ||
        field.analysis_identity != first.analysis_identity ||
        field.id != first.id || field.incarnation != first.incarnation ||
        ((field.id == format::Identity{}) !=
         (field.incarnation == format::Identity{})) ||
        (field.id == format::Identity{} && writer_id == format::Identity{})) {
      return tl::make_unexpected(
          Status::InvalidArgument("Incompatible run source identity"));
    }
    (*segment_rows)[i] = source->statistics().rows;
    end += source->statistics().rows;
  }
  return static_cast<uint32_t>(end - begin);
}

}  // namespace

format::Identity NewFtsIdentity() {
  // The same portable UUID provider is used for immutable sealed generations.
  format::Sha256 hash;
  hash.update(rocksdb::Env::Default()->GenerateUniqueId());
  return IdentityPrefix(hash.digest());
}

Result<FtsRun::Ptr> FtsRun::Write(
    const std::string &segment_path, bool mmap, uint64_t segment_id,
    uint32_t block_id, uint32_t parent_begin,
    const std::vector<std::vector<FtsFieldSource::Ptr>> &sources,
    const FtsRetirementQueue::Pointer &retirement,
    const std::shared_ptr<FtsReadBudget> &read_budget,
    const format::Identity &writer_id, uint64_t temporary_bytes) {
  return Write(
      segment_path, mmap, segment_id, block_id, parent_begin, sources.size(),
      [&](uint32_t field) { return sources[field]; }, retirement, read_budget,
      writer_id, temporary_bytes);
}

Result<FtsRun::Ptr> FtsRun::Write(
    const std::string &segment_path, bool mmap, uint64_t segment_id,
    uint32_t block_id, uint32_t parent_begin, size_t field_count,
    const OpenField &open_field, const FtsRetirementQueue::Pointer &retirement,
    const std::shared_ptr<FtsReadBudget> &read_budget,
    const format::Identity &writer_id, uint64_t temporary_bytes) {
  if (!retirement || !read_budget || segment_path.empty()) {
    return tl::make_unexpected(
        Status::InvalidArgument("Missing run workspace"));
  }
  if (field_count == 0 || field_count > format::kMaxFields || !open_field) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid run field count"));
  }
  FtsRunDescriptor descriptor;
  descriptor.id = NewFtsIdentity();
  descriptor.generation = 1;
  descriptor.parent_begin = parent_begin;
  const auto path = FileHelper::MakeFtsRunPath(
      segment_path, block_id, descriptor.generation, descriptor.id);
  const auto reservation = path + ".reserved";
  const auto pending = path + ".pending";
  std::error_code error;
  if (!std::filesystem::create_directory(
          ailego::FileHelper::PathFromUtf8(reservation), error)) {
    return tl::make_unexpected(
        Status{StatusCode::UNAVAILABLE, "Cannot reserve FTS run"});
  }
  auto reserved_owner = retirement->track(reservation, true, true);
  for (const auto &name : {path, pending}) {
    bool exists =
        std::filesystem::exists(ailego::FileHelper::PathFromUtf8(name), error);
    if (error || exists) {
      return tl::make_unexpected(
          Status{StatusCode::UNAVAILABLE, "FTS run path exists"});
    }
  }
  auto owner = retirement->track(path, false, true);
  auto pending_owner = retirement->track(pending, false, true);
  auto workspace =
      std::make_shared<FtsSpoolWorkspace>(reservation, temporary_bytes, 16);
  {
    auto dumper = core::IndexFactory::CreateDumper("FileDumper");
    if (!dumper || dumper->create(pending) != 0) {
      return tl::make_unexpected(
          Status{StatusCode::UNAVAILABLE, "Cannot create FTS run"});
    }
    auto writer =
        FtsContainerWriter::Create(std::move(dumper), 4096, workspace);
    if (!writer) {
      return tl::make_unexpected(writer.error());
    }
    format::Metadata metadata;
    metadata.required_features = format::kRunLocalDocids;
    metadata.base_id = descriptor.id;
    metadata.base_generation = descriptor.generation;
    metadata.source_segment_id = segment_id;
    metadata.block_id = block_id;
    metadata.parent_row_begin = parent_begin;
    std::vector<uint32_t> segment_rows;
    for (uint32_t f = 0; f < field_count; ++f) {
      auto sources = open_field(f);
      if (!sources) {
        return tl::make_unexpected(sources.error());
      }
      auto rows = ValidateField(segment_id, block_id, parent_begin, f, *sources,
                                writer_id, &segment_rows);
      if (!rows) {
        return tl::make_unexpected(rows.error());
      }
      descriptor.rows = *rows;
      metadata.row_count = *rows;
      auto field = sources->front()->metadata();
      // Import and active sources enter this writing field's incarnation.
      // Coalescing already-bound run sources preserves that identity.
      if (!dynamic_cast<const RunSource *>(sources->front().get()) &&
          writer_id != format::Identity{}) {
        field.id = FieldIdentity(writer_id, "field", f);
        field.incarnation = FieldIdentity(writer_id, "incarnation", f);
      }
      // Ordinals bind to the new base UUID and dictionary generation.
      field.dictionary_generation = descriptor.generation;
      auto frozen = sources->size() == 1
                        ? dynamic_cast<const FtsFrozenFieldSource *>(
                              sources->front().get())
                        : nullptr;
      auto written =
          frozen ? frozen->write_field(std::move(field), writer->get(), f)
                 : MergeFtsField(*sources, {}, FtsMergePolicy::kCoalesceRuns,
                                 std::move(field), writer->get(), f);
      if (!written) {
        return tl::make_unexpected(written.error());
      }
      metadata.fields.push_back(std::move(*written));
    }
    auto status = (*writer)->finish(metadata);
    if (!status.ok()) {
      return tl::make_unexpected(status);
    }
  }
  auto file = FtsContainerReader::Open(pending, mmap, read_budget, owner);
  if (!file) {
    return tl::make_unexpected(file.error());
  }
  descriptor.file_size = std::filesystem::file_size(
      ailego::FileHelper::PathFromUtf8(pending), error);
  if (error) {
    return tl::make_unexpected(
        Status{StatusCode::UNAVAILABLE, "Cannot stat prepared FTS run"});
  }
  descriptor.metadata_digest = (*file)->metadata_digest();
  auto run =
      OpenFile(path, std::move(*file), segment_id, block_id, descriptor, owner);
  if (!run) {
    return run;
  }
  auto published = DurableFile::PublishPrepared(pending, path);
  if (!published.status.ok()) {
    return tl::make_unexpected(published.status);
  }
  (*run)->spool_stats_ = workspace->stats();
  return run;
}

Result<FtsRun::Ptr> FtsRun::Open(
    const std::string &segment_path, bool mmap, uint64_t segment_id,
    uint32_t block_id, const FtsRunDescriptor &descriptor,
    const FtsRetirementQueue::Pointer &retirement,
    const std::shared_ptr<FtsReadBudget> &read_budget) {
  if (!retirement || !read_budget || segment_path.empty()) {
    return tl::make_unexpected(
        Status::InvalidArgument("Missing run workspace"));
  }
  const auto path = FileHelper::MakeFtsRunPath(
      segment_path, block_id, descriptor.generation, descriptor.id);
  std::error_code error;
  const auto size =
      std::filesystem::file_size(ailego::FileHelper::PathFromUtf8(path), error);
  if (error || size != descriptor.file_size) {
    return tl::make_unexpected(
        Status::InternalError("Missing FTS run or file size mismatch"));
  }
  auto owner = retirement->track(path, false, false);
  auto file = FtsContainerReader::Open(path, mmap, read_budget, owner);
  if (!file) {
    return tl::make_unexpected(file.error());
  }
  return OpenFile(path, std::move(*file), segment_id, block_id, descriptor,
                  std::move(owner));
}

Result<FtsRun::Ptr> FtsRun::OpenFile(const std::string &path,
                                     FtsContainerReader::Pointer container,
                                     uint64_t segment_id, uint32_t block_id,
                                     const FtsRunDescriptor &descriptor,
                                     FtsFileOwner::Pointer owner) {
  const auto &metadata = container->metadata();
  if (!metadata.parent_row_begin || metadata.source_segment_id != segment_id ||
      metadata.block_id != block_id || metadata.fields.empty() ||
      descriptor.id != metadata.base_id || descriptor.generation == 0 ||
      descriptor.generation != metadata.base_generation ||
      descriptor.rows != metadata.row_count ||
      descriptor.parent_begin != *metadata.parent_row_begin ||
      descriptor.metadata_digest != container->metadata_digest()) {
    return tl::make_unexpected(
        Status::InternalError("FTS run identity mismatch"));
  }
  auto run = std::make_shared<FtsRun>();
  run->fields_.reserve(metadata.fields.size());
  for (uint32_t field = 0; field < metadata.fields.size(); ++field) {
    auto opened = FtsFieldReader::Open(container, field);
    if (!opened) {
      return tl::make_unexpected(opened.error());
    }
    run->fields_.push_back(std::move(*opened));
  }
  run->path_ = path;
  run->descriptor_ = descriptor;
  owner->register_bytes(descriptor.file_size);
  run->owner_ = std::move(owner);
  run->container_ = std::move(container);
  return run;
}

Result<FtsSegmentReader::Ptr> FtsRun::reader(uint32_t field) const {
  if (field >= fields_.size()) {
    return tl::make_unexpected(Status::InvalidArgument("Missing run field"));
  }
  return OpenCustomFtsReader(fields_[field]);
}

Result<FtsFieldSource::Ptr> FtsRun::source(uint32_t field) const {
  if (field >= fields_.size()) {
    return tl::make_unexpected(Status::InvalidArgument("Missing run field"));
  }
  auto source = OpenCustomFtsFieldSource(fields_[field]);
  if (!source) {
    return tl::make_unexpected(source.error());
  }
  return std::make_shared<RunSource>(std::move(*source), container_, field);
}

bool FtsRun::SourceMatches(const FtsFieldSource &source, uint64_t segment_id,
                           uint32_t block_id, uint64_t begin, uint32_t field) {
  const auto *run = dynamic_cast<const RunSource *>(&source);
  return run && run->matches(segment_id, block_id, begin, field);
}

bool FtsRun::matches_field(uint32_t field, const std::string &name,
                           std::string_view analysis_identity,
                           const format::Identity &writer_id) const {
  if (field >= container_->metadata().fields.size()) {
    return false;
  }
  const auto &info = container_->metadata().fields[field];
  return info.name == name && info.analysis_identity == analysis_identity &&
         info.id == FieldIdentity(writer_id, "field", field) &&
         info.incarnation == FieldIdentity(writer_id, "incarnation", field);
}

}  // namespace zvec::fts
