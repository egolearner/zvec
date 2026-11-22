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

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <zvec/db/status.h>


namespace zvec {

class WalFile;

using WalFilePtr = std::shared_ptr<WalFile>;

struct WalOptions {
  uint32_t max_docs_wal_flush{0};
  bool create_new{false};
  bool read_only{false};
  uint64_t epoch{0};  // Zero selects the legacy forward-block WAL.
};

struct WalReadResult {
  enum class State { kRecord, kEnd, kTruncatedTail };
  State state;
  std::string data;
};

// The serialized Doc is borrowed while encoding/decoding. UINT64_MAX and
// UINT32_MAX mark absent IDs. The enclosing epoch frame checksums all bytes.
struct WalOperation {
  enum class Kind : uint32_t { kInsert = 1, kReplace = 2, kDelete = 3 };
  Kind kind{Kind::kInsert};
  uint64_t epoch{0};
  uint64_t sequence{0};
  std::array<uint8_t, 32> schema_identity{};
  uint64_t old_id{UINT64_MAX};
  uint64_t new_id{UINT64_MAX};
  uint32_t parent_row{UINT32_MAX};
  std::string_view payload;

  Result<std::string> encode() const;
  static Result<WalOperation> Decode(std::string_view bytes);
};

class WalFile {
 public:
  //! Constructor
  WalFile() = default;

  //! Destructor
  virtual ~WalFile() = default;  // LCOV_EXCL_LINE

  //! Create an instance
  static WalFilePtr Create(const std::string &wal_path);

  //! Crate an instance and open
  static int CreateAndOpen(const std::string &wal_path,
                           const WalOptions &wal_options, WalFilePtr *wal_file);

 public:
  virtual int append(std::string &&data) = 0;
  virtual int prepare_for_read() = 0;
  virtual std::string next() = 0;
  virtual Result<WalReadResult> next_checked() = 0;
  virtual Status discard_truncated_tail() = 0;

 public:
  //! Open and initialize WalFile
  virtual int open(const WalOptions &wal_options) = 0;

  //! Close WalFile
  virtual int close() = 0;

  //! Remove wal disk file
  virtual int remove() = 0;

  //! Flush WalFile's memory to disk file
  virtual int flush() = 0;

  virtual bool has_record() = 0;
  virtual uint64_t size_bytes() const = 0;
};

};  // namespace zvec
