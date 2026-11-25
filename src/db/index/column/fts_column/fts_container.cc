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

#include "fts_container.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <zvec/ailego/io/file.h>
#include <zvec/core/framework/index_factory.h>
#include "core/utility/utility_params.h"
#include "fts_stream.h"

namespace zvec::fts {
namespace {

constexpr size_t kChecksumEntries = 128;
constexpr size_t kChecksumPayload = 4096;
constexpr size_t kChecksumPage = kChecksumPayload + 32;

size_t PayloadLimit() {
  // IndexPacker uses size_t internally. Reserve the worst allowed outer table,
  // version string, alignment and footer before writing any section bytes.
  const size_t tail = (format::kMaxSections + 3) *
                          (sizeof(core::IndexFormat::SegmentMeta) + 256) +
                      std::strlen(core::IndexVersion::Details()) + 64 +
                      sizeof(core::IndexFormat::MetaFooter);
  return std::numeric_limits<size_t>::max() - tail;
}

Status IoError() {
  return {StatusCode::UNAVAILABLE,
          "FTS container I/O failed or short read/write"};
}

Status Corrupt() {
  return Status::InternalError("FTS container range/checksum is corrupt");
}

Status Exhausted() {
  return {StatusCode::RESOURCE_EXHAUSTED,
          "FTS range workspace budget exceeded"};
}

Status StorageError(int code) {
  if (code == core::IndexError_NoMemory) {
    return Exhausted();
  }
  if (code == core::IndexError_UnpackIndex ||
      code == core::IndexError_InvalidLength) {
    return Corrupt();
  }
  if (code == core::IndexError_Unsupported) {
    return Status::NotSupported("Unsupported FTS storage operation");
  }
  return IoError();
}

// Generic IndexStorage opens its outer directory before the FTS metadata is
// available. Validate the immutable FileDumper envelope and its allocation
// bounds first, including C-string names consumed by IndexUnpacker. Published
// generations must not be modified or replaced while readers are opening them.
Status CheckEnvelope(const std::string &path) {
  using Layout = core::IndexFormat;
  ailego::File file;
  if (!file.open(path.c_str(), true)) {
    return IoError();
  }
  const uint64_t size = file.size();
  if (size > static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())) {
    return Exhausted();
  }
  if (size < sizeof(Layout::MetaHeader) + sizeof(Layout::MetaFooter)) {
    return Corrupt();
  }
  Layout::MetaHeader header{};
  Layout::MetaFooter footer{};
  const uint64_t footer_offset = size - sizeof(footer);
  if (file.read(0, &header, sizeof(header)) != sizeof(header) ||
      file.read(static_cast<ssize_t>(footer_offset), &footer, sizeof(footer)) !=
          sizeof(footer)) {
    return IoError();
  }
  const uint32_t header_crc = header.header_crc;
  const uint32_t footer_crc = footer.footer_crc;
  header.header_crc = footer.footer_crc = 0;
  if (ailego::Crc32c::Hash(&header, sizeof(header), 0) != header_crc ||
      ailego::Crc32c::Hash(&footer, sizeof(footer), 0) != footer_crc ||
      header.meta_header_size != sizeof(header) ||
      header.meta_footer_size != sizeof(footer)) {
    return Corrupt();
  }
  if (header.version != Layout::FORMAT_VERSION || header.revision != 0 ||
      header.meta_footer_offset != static_cast<uint32_t>(0 - sizeof(footer)) ||
      header.content_offset != sizeof(header) ||
      footer.next_meta_header_offset != 0) {
    return Status::NotSupported("Unsupported FTS outer container layout");
  }
  constexpr size_t max_sections = format::kMaxSections + 3;
  constexpr size_t directory_budget =
      max_sections * (sizeof(Layout::SegmentMeta) + 257) + 31;
  if (footer.segment_count > max_sections ||
      footer.segments_meta_size > directory_budget) {
    return Exhausted();
  }
  const uint64_t records_size =
      static_cast<uint64_t>(footer.segment_count) * sizeof(Layout::SegmentMeta);
  if (footer.total_size != size || footer.segment_count < 3 ||
      footer.segments_meta_size < records_size ||
      footer.segments_meta_size > footer_offset - sizeof(header) ||
      footer.content_padding_size >= 32) {
    return Corrupt();
  }
  const uint64_t directory_offset = footer_offset - footer.segments_meta_size;
  if (!format::ValidRange(sizeof(header), footer.content_size,
                          directory_offset) ||
      directory_offset - sizeof(header) - footer.content_size !=
          footer.content_padding_size) {
    return Corrupt();
  }
  std::string directory(footer.segments_meta_size, '\0');
  if (file.read(static_cast<ssize_t>(directory_offset), directory.data(),
                directory.size()) != directory.size()) {
    return IoError();
  }
  if (ailego::Crc32c::Hash(directory.data(), directory.size(), 0) !=
      footer.segments_meta_crc) {
    return Corrupt();
  }
  std::set<std::string> names;
  uint64_t end = 0;
  for (uint32_t i = 0; i < footer.segment_count; ++i) {
    Layout::SegmentMeta segment{};
    std::memcpy(&segment, directory.data() + i * sizeof(segment),
                sizeof(segment));
    if (segment.segment_id_offset < records_size ||
        segment.segment_id_offset >= directory.size() ||
        segment.data_index != end ||
        !format::ValidRange(end, segment.data_size, footer.content_size)) {
      return Corrupt();
    }
    end += segment.data_size;
    if (!format::ValidRange(end, segment.padding_size, footer.content_size)) {
      return Corrupt();
    }
    end += segment.padding_size;
    const char *start = directory.data() + segment.segment_id_offset;
    const size_t limit =
        std::min<size_t>(257, directory.size() - segment.segment_id_offset);
    const auto *terminator =
        static_cast<const char *>(std::memchr(start, '\0', limit));
    if (!terminator || terminator == start) {
      return Corrupt();
    }
    const std::string name(start, terminator);
    if (!names.insert(name).second) {
      return Corrupt();
    }
    if ((name == "IndexVersion" && segment.data_size > 4096) ||
        (name == "fts.meta" && segment.data_size > format::kMetadataBudget)) {
      return Exhausted();
    }
  }
  if (end != footer.content_size || names.count("IndexVersion") == 0 ||
      names.count("fts.meta") == 0 || names.count("checksums") == 0) {
    return Corrupt();
  }
  return Status::OK();
}

}  // namespace

FtsContainerWriter::FtsContainerWriter(
    core::IndexDumper::Pointer dumper, uint32_t granule,
    std::unique_ptr<FtsSpool> spool,
    std::shared_ptr<FtsSpoolWorkspace> workspace)
    : dumper_(std::move(dumper)),
      granule_(granule),
      spool_(std::move(spool)),
      workspace_(std::move(workspace)),
      output_size_(dumper_->size()) {}

FtsContainerWriter::~FtsContainerWriter() = default;

Result<std::unique_ptr<FtsContainerWriter>> FtsContainerWriter::Create(
    core::IndexDumper::Pointer dumper, uint32_t layout_granule,
    std::shared_ptr<FtsSpoolWorkspace> workspace) {
  if (!dumper || layout_granule < 1024 || layout_granule > 65536 ||
      (layout_granule & (layout_granule - 1)) != 0) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid FTS container writer"));
  }
  if (dumper->size() > PayloadLimit()) {
    return tl::make_unexpected(Exhausted());
  }
  if (workspace) {
    // Header, version string, alignment of the version, payload and table,
    // generated section directory entries and footer. User sections charge
    // their own record and NUL-terminated name before appending them.
    const auto framing =
        dumper->size() + std::strlen(core::IndexVersion::Details()) + 96 +
        sizeof(core::IndexFormat::MetaFooter) +
        3 * sizeof(core::IndexFormat::SegmentMeta) + sizeof("IndexVersion") +
        sizeof("fts.meta") + sizeof("checksums");
    if (!workspace->take(framing)) {
      return tl::make_unexpected(
          Status{StatusCode::RESOURCE_EXHAUSTED,
                 "FTS candidate framing exceeds disk budget"});
    }
  }
  auto spool = FtsSpool::Create(workspace);
  if (!spool) {
    return tl::make_unexpected(spool.error());
  }
  return std::unique_ptr<FtsContainerWriter>(
      new FtsContainerWriter(std::move(dumper), layout_granule,
                             std::move(*spool), std::move(workspace)));
}

Status FtsContainerWriter::begin_section(const std::string &name) {
  if (!status_.ok()) {
    return status_;
  }
  if (finished_ || !active_name_.empty()) {
    return Status::FailedPrecondition(
        "FTS writer is finished or section is open");
  }
  if (name.empty() || name == "fts.meta" || name == "checksums" ||
      name == "IndexVersion" || name.find('\0') != std::string::npos) {
    return Status::InvalidArgument("Invalid FTS section name");
  }
  if (name.size() > 256 || sections_.size() >= format::kMaxSections) {
    return Exhausted();
  }
  for (const auto &section : sections_) {
    if (section.name == name) {
      return Status::InvalidArgument("Duplicate FTS section name");
    }
  }
  active_name_ = name;
  section_length_ = 0;
  first_checksum_ = checksum_count_;
  return Status::OK();
}

Status FtsContainerWriter::checksum_page() {
  format::Sha256 hash;
  auto status = hash.update(
      std::string_view(reinterpret_cast<const char *>(checksum_page_.data()),
                       checksum_page_.size()));
  if (!status.ok()) {
    return status_ = status;
  }
  const auto digest = hash.digest();
  if (checksum_bytes_ > std::numeric_limits<size_t>::max() - kChecksumPage) {
    return status_ = Exhausted();
  }
  status =
      spool_->append({reinterpret_cast<const char *>(checksum_page_.data()),
                      checksum_page_.size()});
  if (status.ok()) {
    status = spool_->append(
        {reinterpret_cast<const char *>(digest.data()), digest.size()});
  }
  if (!status.ok()) {
    return status_ = status;
  }
  checksum_bytes_ += kChecksumPage;
  checksum_page_.fill(0);
  page_entries_ = 0;
  return Status::OK();
}

Status FtsContainerWriter::checksum_chunk() {
  const auto digest = chunk_hash_.digest();
  std::memcpy(checksum_page_.data() + page_entries_ * digest.size(),
              digest.data(), digest.size());
  ++page_entries_;
  ++checksum_count_;
  chunk_size_ = 0;
  chunk_hash_ = format::Sha256();
  if (page_entries_ == kChecksumEntries) {
    return checksum_page();
  }
  return Status::OK();
}

Status FtsContainerWriter::write_bytes(std::string_view bytes) {
  if (bytes.size() > PayloadLimit() - output_size_) {
    return status_ = Exhausted();
  }
  if (workspace_ && !workspace_->take(bytes.size())) {
    return status_ = {StatusCode::RESOURCE_EXHAUSTED,
                      "FTS candidate exceeds disk budget"};
  }
  if (dumper_->write(bytes.data(), bytes.size()) != bytes.size()) {
    return status_ = IoError();
  }
  output_size_ += bytes.size();
  return Status::OK();
}

Status FtsContainerWriter::append(std::string_view bytes) {
  if (!status_.ok()) {
    return status_;
  }
  if (active_name_.empty() || finished_) {
    return Status::FailedPrecondition("FTS writer has no open section");
  }
  if (bytes.size() > std::numeric_limits<size_t>::max() - section_length_) {
    return status_ = Exhausted();
  }
  while (!bytes.empty()) {
    const size_t n = std::min(bytes.size(), granule_ - chunk_size_);
    if (!write_bytes(bytes.substr(0, n)).ok()) {
      return status_;
    }
    status_ = chunk_hash_.update(bytes.substr(0, n));
    if (!status_.ok()) {
      return status_;
    }
    chunk_size_ += n;
    section_length_ += n;
    bytes.remove_prefix(n);
    if (chunk_size_ == granule_) {
      status_ = checksum_chunk();
      if (!status_.ok()) {
        return status_;
      }
    }
  }
  return Status::OK();
}

Status FtsContainerWriter::end_section() {
  if (!status_.ok()) {
    return status_;
  }
  if (active_name_.empty() || finished_) {
    return Status::FailedPrecondition("FTS writer has no open section");
  }
  if (chunk_size_ != 0) {
    status_ = checksum_chunk();
    if (!status_.ok()) {
      return status_;
    }
  }
  if (workspace_ && !workspace_->take(sizeof(core::IndexFormat::SegmentMeta) +
                                      active_name_.size() + 1)) {
    return status_ = {StatusCode::RESOURCE_EXHAUSTED,
                      "FTS directory exceeds disk budget"};
  }
  if (dumper_->append(active_name_, static_cast<size_t>(section_length_), 0,
                      0) != 0) {
    return status_ = IoError();
  }
  sections_.push_back({active_name_, section_length_, first_checksum_});
  active_name_.clear();
  return Status::OK();
}

Status FtsContainerWriter::finish(const format::Metadata &metadata) {
  if (!status_.ok()) {
    return status_;
  }
  if (finished_ || !active_name_.empty()) {
    return Status::FailedPrecondition(
        "FTS writer is finished or section is open");
  }
  if (!metadata.sections.empty() || metadata.layout_granule != granule_) {
    return Status::InvalidArgument("FTS metadata disagrees with writer");
  }
  // Validate the caller's variable-size metadata before copying it into the
  // bounded candidate. Section descriptors are already limited by this writer.
  const auto checked = format::EncodeMetadata(metadata);
  if (!checked) {
    return checked.error();
  }
  format::Metadata candidate = metadata;
  candidate.sections = sections_;
  const auto encoded = format::EncodeMetadata(candidate);
  if (!encoded) {
    return encoded.error();
  }
  if (page_entries_ != 0) {
    status_ = checksum_page();
    if (!status_.ok()) {
      return status_;
    }
  }
  // A bounded copy buffer, independent of the size of the checksum directory.
  for (uint64_t offset = 0; offset < checksum_bytes_;) {
    const size_t n = static_cast<size_t>(
        std::min<uint64_t>(checksum_bytes_ - offset, 16384));
    auto bytes = spool_->read(offset, n);
    if (!bytes) {
      return status_ = bytes.error();
    }
    if (!write_bytes(*bytes).ok()) {
      return status_;
    }
    offset += n;
  }
  if (dumper_->append("checksums", static_cast<size_t>(checksum_bytes_), 0,
                      0) != 0) {
    return status_ = IoError();
  }
  if (!write_bytes(*encoded).ok()) {
    return status_;
  }
  if (dumper_->append("fts.meta", encoded->size(), 0, 0) != 0 ||
      dumper_->close() != 0) {
    return status_ = IoError();
  }
  finished_ = true;
  return Status::OK();
}

struct FtsContainerReader::Lease {
  std::shared_ptr<FtsContainerReader> owner;
  size_t bytes;
  Lease(std::shared_ptr<FtsContainerReader> reader, size_t charge)
      : owner(std::move(reader)), bytes(charge) {}
  ~Lease() {
    owner->budget_->used_.fetch_sub(bytes, std::memory_order_relaxed);
  }
};

FtsReadRange::FtsReadRange(FtsReadRange &&other) noexcept {
  swap(other);
}

FtsReadRange &FtsReadRange::operator=(FtsReadRange &&other) noexcept {
  if (this != &other) {
    FtsReadRange replacement(std::move(other));
    swap(replacement);
    // Destroy old native pins before releasing their budget and file owner.
  }
  return *this;
}

void FtsReadRange::swap(FtsReadRange &other) noexcept {
  using std::swap;
  swap(lease_, other.lease_);
  swap(segment_, other.segment_);
  swap(block_, other.block_);
  swap(length_, other.length_);
}

bool FtsReadBudget::try_acquire(size_t bytes) {
  size_t current = used_.load(std::memory_order_relaxed);
  do {
    if (bytes > limit_ - current) {
      return false;
    }
  } while (!used_.compare_exchange_weak(current, current + bytes,
                                        std::memory_order_relaxed));
  return true;
}

Result<std::shared_ptr<void>> FtsReadBudget::reserve(size_t bytes) {
  if (!try_acquire(bytes)) {
    return tl::make_unexpected(Exhausted());
  }
  return std::shared_ptr<void>(
      this, [owner = shared_from_this(), bytes](void *) {
        owner->used_.fetch_sub(bytes, std::memory_order_relaxed);
      });
}

Result<std::shared_ptr<FtsContainerReader::Lease>> FtsContainerReader::reserve(
    size_t bytes) {
  if (!budget_->try_acquire(bytes)) {
    return tl::make_unexpected(Exhausted());
  }
  return std::make_shared<Lease>(shared_from_this(), bytes);
}

Result<FtsContainerReader::Pointer> FtsContainerReader::Open(
    const std::string &path, bool enable_mmap, size_t read_budget,
    std::shared_ptr<void> file_owner) {
  return Open(path, enable_mmap, std::make_shared<FtsReadBudget>(read_budget),
              std::move(file_owner));
}

Result<FtsContainerReader::Pointer> FtsContainerReader::Open(
    const std::string &path, bool enable_mmap,
    std::shared_ptr<FtsReadBudget> read_budget,
    std::shared_ptr<void> file_owner) {
  if (!read_budget) {
    return tl::make_unexpected(
        Status::InvalidArgument("Missing FTS read budget"));
  }
  const auto envelope = CheckEnvelope(path);
  if (!envelope.ok()) {
    return tl::make_unexpected(envelope);
  }
  Pointer reader(new FtsContainerReader());
  reader->file_owner_ = std::move(file_owner);
  reader->budget_ = std::move(read_budget);
  reader->buffer_pool_ = !enable_mmap;
  reader->storage_ = core::IndexFactory::CreateStorage(
      enable_mmap ? "MMapFileReadStorage" : "BufferReadStorage");
  if (!reader->storage_) {
    return tl::make_unexpected(
        Status::NotSupported("FTS storage backend unavailable"));
  }
  ailego::Params params;
  if (!enable_mmap) {
    params.set(core::BUFFER_READ_STORAGE_WARMUP_MODE,
               core::BUFFER_READ_STORAGE_WARMUP_NONE);
  }
  const int initialized = reader->storage_->init(params);
  if (initialized != 0) {
    return tl::make_unexpected(StorageError(initialized));
  }
  const int opened = reader->storage_->open(path, false);
  if (opened != 0) {
    return tl::make_unexpected(StorageError(opened));
  }
  const auto meta_section = reader->storage_->get("fts.meta");
  if (!meta_section) {
    return tl::make_unexpected(
        Status::NotSupported("Missing FTS container metadata"));
  }
  if (meta_section->data_size() > format::kMetadataBudget) {
    return tl::make_unexpected(Exhausted());
  }
  std::string bytes(meta_section->data_size(), '\0');
  if (meta_section->fetch(0, bytes.data(), bytes.size()) != bytes.size()) {
    return tl::make_unexpected(IoError());
  }
  auto metadata = format::DecodeMetadata(bytes);
  if (!metadata) {
    return tl::make_unexpected(metadata.error());
  }
  format::Sha256 metadata_hash;
  auto hashed = metadata_hash.update(bytes);
  if (!hashed.ok()) {
    return tl::make_unexpected(hashed);
  }
  reader->metadata_digest_ = metadata_hash.digest();
  reader->metadata_ = std::move(*metadata);
  uint64_t checksum_count = 0;
  for (const auto &section : reader->metadata_.sections) {
    if (section.length > std::numeric_limits<size_t>::max()) {
      return tl::make_unexpected(
          Status::NotSupported("FTS section exceeds platform size"));
    }
    const auto physical = reader->storage_->get(section.name);
    if (!physical || physical->data_size() != section.length) {
      return tl::make_unexpected(Corrupt());
    }
    checksum_count += section.length / reader->metadata_.layout_granule +
                      (section.length % reader->metadata_.layout_granule != 0);
  }
  const uint64_t checksum_pages = checksum_count / kChecksumEntries +
                                  (checksum_count % kChecksumEntries != 0);
  reader->checksums_ = reader->storage_->get("checksums");
  if (checksum_pages > std::numeric_limits<size_t>::max() / kChecksumPage) {
    return tl::make_unexpected(
        Status::NotSupported("FTS checksum directory exceeds platform size"));
  }
  if (!reader->checksums_ ||
      reader->checksums_->data_size() != checksum_pages * kChecksumPage) {
    return tl::make_unexpected(Corrupt());
  }
  return reader;
}

Status FtsContainerReader::verify_chunk(const format::SectionMetadata &meta,
                                        uint64_t chunk,
                                        std::string_view bytes) {
  const uint64_t checksum = meta.first_checksum + chunk;
  const size_t checksum_offset =
      static_cast<size_t>(checksum / kChecksumEntries) * kChecksumPage;
  core::IndexStorage::MemoryBlock checksums;
  if (checksums_->read(checksum_offset, checksums, kChecksumPage) !=
          kChecksumPage ||
      checksums.data() == nullptr) {
    return IoError();
  }
  const auto *data = static_cast<const char *>(checksums.data());
  format::Sha256 directory_hash;
  auto status = directory_hash.update({data, kChecksumPayload});
  if (!status.ok()) {
    return status;
  }
  hashed_bytes_.fetch_add(kChecksumPayload, std::memory_order_relaxed);
  const auto directory_digest = directory_hash.digest();
  if (std::memcmp(directory_digest.data(), data + kChecksumPayload, 32) != 0) {
    return Corrupt();
  }
  format::Sha256 hash;
  status = hash.update(bytes);
  if (!status.ok()) {
    return status;
  }
  hashed_bytes_.fetch_add(bytes.size(), std::memory_order_relaxed);
  const auto digest = hash.digest();
  if (std::memcmp(digest.data(), data + (checksum % kChecksumEntries) * 32,
                  32) != 0) {
    return Corrupt();
  }
  verified_pages_.fetch_add(1, std::memory_order_relaxed);
  return Status::OK();
}

struct FtsContainerReader::ScanWindows {
  static constexpr size_t kBytes = 32768;
  struct Window {
    std::string name;
    uint64_t size{0}, begin{0};
    std::shared_ptr<FtsReadRange> range;
  };
  std::array<Window, 8> sections;
  std::mutex mutex;
};

FtsContainerReader::Pointer FtsContainerReader::scan_view() {
  if (scan_base_) {
    return scan_base_->scan_view();
  }
  // Leave at least half the shared budget for posting cursors, queries and
  // overlapping ranges. Tight budgets retain only the normal cursor windows.
  const size_t page =
      std::max<size_t>(metadata_.layout_granule,
                       std::max(ailego::kVectorPageSize, kChecksumPage));
  if (budget_->limit_ < 2 * 16 * 8 * (4 * page + ScanWindows::kBytes)) {
    return shared_from_this();
  }
  Pointer view(new FtsContainerReader());
  view->scan_base_ = shared_from_this();
  view->scan_windows_ = std::make_shared<ScanWindows>();
  view->storage_ = storage_;
  view->buffer_pool_ = buffer_pool_;
  return view;
}

Result<FtsReadRange> FtsContainerReader::scan_range(const std::string &section,
                                                    uint64_t offset,
                                                    uint64_t length) {
  constexpr size_t kWindow = ScanWindows::kBytes;
  if (length == 0 || length > kWindow) {
    return scan_base_->read(section, offset, length);
  }
  std::lock_guard lock(scan_windows_->mutex);
  for (auto &window : scan_windows_->sections) {
    if (window.name.empty()) {
      const auto &sections = scan_base_->metadata_.sections;
      auto found =
          std::find_if(sections.begin(), sections.end(),
                       [&](const auto &item) { return item.name == section; });
      if (found == sections.end()) {
        return tl::make_unexpected(
            Status::NotFound("Missing FTS scan section"));
      }
      window.name = section;
      window.size = found->length;
    }
    if (window.name != section) {
      continue;
    }
    if (!format::ValidRange(offset, length, window.size)) {
      return tl::make_unexpected(Corrupt());
    }
    if (!window.range || offset < window.begin ||
        !format::ValidRange(offset - window.begin, length,
                            window.range->view().size())) {
      window.range.reset();
      window.begin = offset;
      auto bytes = scan_base_->read(
          section, offset, std::min<uint64_t>(kWindow, window.size - offset));
      if (!bytes) {
        if (bytes.error().code() == StatusCode::RESOURCE_EXHAUSTED) {
          return scan_base_->read(section, offset, length);
        }
        return tl::make_unexpected(bytes.error());
      }
      window.range = std::make_shared<FtsReadRange>(std::move(*bytes));
    }
    FtsReadRange slice;
    slice.lease_ = window.range;
    slice.length_ = length;
    // The storage API takes void* for a borrowed block, but FtsReadRange
    // only exports a read-only string_view.
    auto *data = const_cast<char *>(window.range->view().data());
    slice.block_ = core::IndexStorage::MemoryBlock::MakeBorrowedView(
        data + (offset - window.begin));
    return slice;
  }
  return scan_base_->read(section, offset, length);
}

Result<FtsReadRange> FtsContainerReader::read(const std::string &section_name,
                                              uint64_t offset,
                                              uint64_t length) {
  if (scan_base_) {
    return scan_range(section_name, offset, length);
  }
  const format::SectionMetadata *meta = nullptr;
  for (const auto &section : metadata_.sections) {
    if (section.name == section_name) {
      meta = &section;
      break;
    }
  }
  if (!meta) {
    return tl::make_unexpected(Status::NotFound("FTS section not found"));
  }
  if (!format::ValidRange(offset, length, meta->length)) {
    return tl::make_unexpected(
        Status::InvalidArgument("FTS range is outside section"));
  }
  FtsReadRange range;
  if (length == 0) {
    return range;
  }
  range.segment_ = storage_->get(section_name);
  if (!range.segment_) {
    return tl::make_unexpected(Corrupt());
  }
  // Retain the conservative range allowance for native page alignment and
  // buffer-mode bypass scratch. Explicit verification also reads a checksum
  // page within this allowance; normal queries only pin the requested data.
  const size_t page =
      std::max<size_t>(metadata_.layout_granule, ailego::kVectorPageSize);
  const size_t overhead = 4 * std::max(page, kChecksumPage);
  uint64_t scratch;
  if (buffer_pool_) {
    // A native-page window can span several checksum chunks while remaining
    // a single pinned backend page. Only physical page crossings need a copy.
    uint64_t within = range.segment_->data_offset() % ailego::kVectorPageSize +
                      offset % ailego::kVectorPageSize;
    if (within >= ailego::kVectorPageSize) {
      within -= ailego::kVectorPageSize;
    }
    scratch = length <= ailego::kVectorPageSize - within ? 0 : length;
  } else {
    const uint64_t first = offset / metadata_.layout_granule;
    const uint64_t last = (offset + length - 1) / metadata_.layout_granule;
    scratch = first == last ? 0 : length;
  }
  if (scratch > std::numeric_limits<size_t>::max() - overhead) {
    return tl::make_unexpected(Exhausted());
  }
  auto lease = reserve(overhead + static_cast<size_t>(scratch));
  if (!lease) {
    return tl::make_unexpected(lease.error());
  }
  range.lease_ = std::move(*lease);
  range.length_ = static_cast<size_t>(length);
  range_reads_.fetch_add(1, std::memory_order_relaxed);
  if (range.segment_->read(static_cast<size_t>(offset), range.block_,
                           range.length_) != range.length_ ||
      range.block_.data() == nullptr) {
    return tl::make_unexpected(IoError());
  }
  return range;
}

Result<FtsReadRange> FtsContainerReader::read_verified_chunk(
    const format::SectionMetadata &section, uint64_t offset) {
  if (scan_base_) {
    return scan_base_->read_verified_chunk(section, offset);
  }
  if (offset >= section.length || offset % metadata_.layout_granule != 0) {
    return tl::make_unexpected(Corrupt());
  }
  auto range = read(
      section.name, offset,
      std::min<uint64_t>(metadata_.layout_granule, section.length - offset));
  if (!range) {
    return range;
  }
  auto status =
      verify_chunk(section, offset / metadata_.layout_granule, range->view());
  if (!status.ok()) {
    return tl::make_unexpected(status);
  }
  return range;
}

Status FtsContainerReader::verify_all() {
  if (scan_base_) {
    return scan_base_->verify_all();
  }
  for (const auto &section : metadata_.sections) {
    uint64_t offset = 0;
    while (offset < section.length) {
      auto range = read_verified_chunk(section, offset);
      if (!range) {
        return range.error();
      }
      offset += range->view().size();
    }
  }
  return Status::OK();
}

}  // namespace zvec::fts
