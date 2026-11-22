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

#include "local_wal_file.h"
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <zvec/ailego/hash/crc32c.h>
#include <zvec/ailego/logger/logger.h>
#include "db/common/error_code.h"
#include "db/common/file_helper.h"
#include "db/common/typedef.h"
#include "db/index/column/fts_column/fts_format.h"

namespace zvec {
namespace {

// Legacy readers accept up to 4 MiB. Epoch readers reserve up to 64 MiB
// including the operation envelope and serialized document.
size_t RecordLimit(uint64_t epoch) {
  return epoch == 0 ? 4ULL << 20 : 64ULL << 20;
}

std::string EpochHeader(uint64_t epoch) {
  std::string bytes("ZVWALE01", 8);
  fts::format::AppendU64(epoch, &bytes);
  bytes.resize(32, '\0');
  fts::format::Sha256 hash;
  hash.update(bytes);
  const auto digest = hash.digest();
  bytes.append(reinterpret_cast<const char *>(digest.data()), digest.size());
  return bytes;
}

}  // namespace

int LocalWalFile::append(std::string &&data) {
  CHECK_STATUS(read_only_, false);
  if (data.empty() || data.size() > RecordLimit(epoch_)) {
    return -1;
  }
  WalRecord record;
  record.length_ = data.size();
  record.crc_ = ailego::Crc32c::Hash(
      reinterpret_cast<const void *>(data.data()), record.length_, 0);
  record.content_ = std::forward<std::string>(data);

  if (write_record(record) < 0) {
    WLOG_ERROR("Wal write record error. record.length_[%zu]",
               (size_t)record.length_);
    return -1;
  }
  // if max_docs_wal_flush_ is 0, no need flush
  if (max_docs_wal_flush_ != 0 && docs_count_ >= max_docs_wal_flush_) {
    if (!file_.flush()) {
      WLOG_ERROR("Wal flush error. docs_count_[%zu] max_docs_wal_flush_[%zu]",
                 (size_t)docs_count_, (size_t)max_docs_wal_flush_);
      return -1;
    }
    docs_count_ = 0;
  }
  return 0;
}

std::string LocalWalFile::next() {
  auto record = next_checked();
  if (record && record->state == WalReadResult::State::kRecord) {
    return std::move(record->data);
  }
  return std::string();
}

int LocalWalFile::open(const WalOptions &wal_option) {
  CHECK_STATUS(opened_, false);
  epoch_ = wal_option.epoch;
  truncated_at_.reset();
  if (wal_option.read_only && wal_option.create_new) {
    WLOG_ERROR("Cannot create a read-only WAL file");
    return -1;
  }
  if (wal_option.create_new) {
    if (FileHelper::FileExists(wal_path_)) {
      WLOG_ERROR("Wal open error. file already exist create_new[%d]",
                 wal_option.create_new);
      return -1;
    }

    if (!file_.create(wal_path_, false)) {
      WLOG_ERROR("Wal create error. create_new[%d]", wal_option.create_new);
      return -1;
    }

    // write wal header
    const auto bytes =
        epoch_ ? EpochHeader(epoch_)
               : std::string(reinterpret_cast<const char *>(&header_),
                             sizeof(header_));
    int write_size = file_.write(bytes.data(), bytes.size());
    if (write_size != sizeof(header_)) {
      WLOG_ERROR("Wal write header error. create_new[%d]",
                 wal_option.create_new);
      file_.close();
      return -1;
    }

  } else {
    if (!FileHelper::FileExists(wal_path_)) {
      WLOG_ERROR("Wal open error. file is not exist create_new[%d]",
                 wal_option.create_new);
      return -1;
    }

    if (!file_.open(wal_path_.c_str(), wal_option.read_only)) {
      WLOG_ERROR("Wal open error. create_new[%d]", wal_option.create_new);
      return -1;
    }

    // Appends start at the end; prepare_for_read() rewinds readers.
    if (epoch_) {
      const auto expected = EpochHeader(epoch_);
      std::string bytes(expected.size(), '\0');
      if (file_.read(0, bytes.data(), bytes.size()) != bytes.size() ||
          bytes != expected) {
        file_.close();
        return -1;
      }
    }
    if (!file_.seek(0, ailego::File::Origin::End)) {
      file_.close();
      return -1;
    }
  }

  max_docs_wal_flush_ = wal_option.max_docs_wal_flush;
  bytes_ = file_.size();
  read_only_ = wal_option.read_only;
  opened_ = true;

  WLOG_INFO("Wal open success. create_new[%d]", wal_option.create_new);
  return 0;
}

int LocalWalFile::close() {
  CHECK_STATUS(opened_, true);
  file_.close();
  WLOG_INFO("Wal close success");
  opened_ = false;
  return 0;
}

int LocalWalFile::remove() {
  CHECK_STATUS(read_only_, false);
  if (opened_) {
    close();
  }
  if (FileHelper::FileExists(wal_path_)) {
    if (!FileHelper::RemoveFile(wal_path_)) {
      return -1;
    }
    WLOG_INFO("Wal remove success.");
  }
  return 0;
}

int LocalWalFile::flush() {
  CHECK_STATUS(opened_, true);
  CHECK_STATUS(read_only_, false);
  if (!file_.flush()) {
    WLOG_ERROR("Wal flush error.");
    return -1;
  }
  return 0;
}

int LocalWalFile::prepare_for_read() {
  CHECK_STATUS(opened_, true);
  if (!file_.seek(0, ailego::File::Origin::Begin)) {
    return -1;
  }
  WalHeader header{};
  int read_size = file_.read((void *)&header, sizeof(header));
  if (read_size != sizeof(header_)) {
    WLOG_ERROR("Wal read header error.");
    return -1;
  }
  if (epoch_) {
    const auto bytes = std::string_view(reinterpret_cast<const char *>(&header),
                                        sizeof(header));
    return bytes == EpochHeader(epoch_) ? 0 : -1;
  }
  if (header.wal_version != 0UL) {
    WLOG_ERROR("Wal version not support error.");
    return -1;
  }
  return 0;
}

//! Return 1 if success or -1 if write error
int LocalWalFile::write_record(WalRecord &record) {
  CHECK_STATUS(opened_, true);

  int write_size = 0;
  int ret = -1;

  std::lock_guard<std::mutex> lock(file_mutex_);
  if (epoch_) {
    std::string frame;
    fts::format::AppendU32(record.length_, &frame);
    fts::format::AppendU32(~record.length_, &frame);
    fts::format::AppendU32(record.crc_, &frame);
    if (file_.write(frame.data(), frame.size()) != frame.size() ||
        file_.write(record.content_.data(), record.length_) != record.length_) {
      return -1;
    }
    ++docs_count_;
    bytes_ += frame.size() + static_cast<uint64_t>(record.length_);
    return 1;
  }
  do {
    write_size = file_.write((const void *)&record.length_, LENGTH_SIZE);
    if (write_size != LENGTH_SIZE) {
      WLOG_ERROR("Wal write error. record.length_ error write_size[%d]",
                 write_size);
      break;
    }

    write_size = file_.write((const void *)&record.crc_, CRC_SIZE);
    if (write_size != CRC_SIZE) {
      WLOG_ERROR("Wal write error. record.crc_ error write_size[%d]",
                 write_size);
      break;
    }

    write_size =
        file_.write((const void *)record.content_.data(), record.length_);
    if (write_size != (int)record.length_) {
      WLOG_ERROR("Wal write error. record.content_ error write_size[%d]",
                 write_size);
      break;
    }
    ret = 1;  // write one record success
    bytes_ += LENGTH_SIZE + CRC_SIZE + static_cast<uint64_t>(record.length_);
    docs_count_++;
  } while (false);

  return ret;
}

Result<WalReadResult> LocalWalFile::next_checked() {
  if (!opened_) {
    return tl::make_unexpected(Status::FailedPrecondition("WAL is closed"));
  }
  using State = WalReadResult::State;
  const auto offset = file_.offset();
  const auto size = file_.size();
  if (offset < 0 || static_cast<uint64_t>(offset) > size) {
    return tl::make_unexpected(Status::InternalError("Invalid WAL offset"));
  }
  const size_t remaining = size - static_cast<size_t>(offset);
  if (!remaining) {
    return WalReadResult{State::kEnd, {}};
  }
  auto truncated = [&]() {
    truncated_at_ = static_cast<size_t>(offset);
    return WalReadResult{State::kTruncatedTail, {}};
  };
  const size_t header_size = epoch_ ? 12 : 8;
  if (remaining < header_size) {
    return truncated();
  }
  char bytes[12];
  if (file_.read(bytes, header_size) != header_size) {
    return tl::make_unexpected(Status::InternalError("Short WAL header read"));
  }
  uint32_t length, checksum;
  if (epoch_) {
    fts::format::Decoder decoder(std::string_view(bytes, header_size));
    length = decoder.u32();
    if (decoder.u32() != ~length) {
      return tl::make_unexpected(
          Status::InternalError("Invalid WAL frame length"));
    }
    checksum = decoder.u32();
  } else {
    std::memcpy(&length, bytes, sizeof(length));
    std::memcpy(&checksum, bytes + sizeof(length), sizeof(checksum));
  }
  if (length == 0 || length > RecordLimit(epoch_)) {
    return tl::make_unexpected(
        Status::InternalError("Invalid WAL record length"));
  }
  if (length > remaining - header_size) {
    return truncated();
  }
  std::string record(length, '\0');
  if (file_.read(record.data(), length) != length) {
    return tl::make_unexpected(Status::InternalError("Short WAL record read"));
  }
  if (ailego::Crc32c::Hash(record.data(), length, 0) != checksum) {
    return tl::make_unexpected(
        Status::InternalError("WAL record checksum mismatch"));
  }
  return WalReadResult{State::kRecord, std::move(record)};
}

Status LocalWalFile::discard_truncated_tail() {
  if (read_only_ || !opened_ || !truncated_at_) {
    return Status::FailedPrecondition("No writable truncated WAL tail");
  }
  if (!file_.truncate(*truncated_at_) || !file_.flush() ||
      !file_.seek(0, ailego::File::Origin::End)) {
    return Status::InternalError("Cannot repair truncated WAL tail");
  }
  bytes_ = *truncated_at_;
  truncated_at_.reset();
  return Status::OK();
}

};  // namespace zvec
