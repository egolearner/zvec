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

#include "wal_file.h"
#include <algorithm>
#include "db/index/column/fts_column/fts_format.h"
#include "local_wal_file.h"

namespace zvec {
namespace {
constexpr std::string_view kOperationMagic = "ZVWALOP1";
constexpr size_t kOperationHeader = 84;

bool Valid(const WalOperation &op) {
  using Kind = WalOperation::Kind;
  return (op.kind == Kind::kInsert || op.kind == Kind::kReplace ||
          op.kind == Kind::kDelete) &&
         op.epoch != 0 && op.sequence != UINT64_MAX &&
         op.schema_identity != std::array<uint8_t, 32>{} &&
         (op.old_id != UINT64_MAX) == (op.kind != Kind::kInsert) &&
         (op.new_id != UINT64_MAX) == (op.kind != Kind::kDelete) &&
         (op.parent_row != UINT32_MAX) == (op.kind != Kind::kDelete) &&
         (op.kind != Kind::kReplace || op.old_id < op.new_id) &&
         !op.payload.empty() &&
         op.payload.size() <= (64u << 20) - kOperationHeader;
}
}  // namespace

Result<std::string> WalOperation::encode() const {
  if (!Valid(*this)) {
    return tl::make_unexpected(
        Status::InvalidArgument("Invalid WAL operation"));
  }
  std::string bytes;
  bytes.reserve(kOperationHeader + payload.size());
  bytes.append(kOperationMagic);
  fts::format::AppendU64(epoch, &bytes);
  fts::format::AppendU64(sequence, &bytes);
  fts::format::AppendU32(static_cast<uint32_t>(kind), &bytes);
  bytes.append(reinterpret_cast<const char *>(schema_identity.data()),
               schema_identity.size());
  fts::format::AppendU64(old_id, &bytes);
  fts::format::AppendU64(new_id, &bytes);
  fts::format::AppendU32(parent_row, &bytes);
  fts::format::AppendU32(static_cast<uint32_t>(payload.size()), &bytes);
  bytes.append(payload);
  return bytes;
}

Result<WalOperation> WalOperation::Decode(std::string_view bytes) {
  fts::format::Decoder decoder(bytes);
  if (decoder.take(kOperationMagic.size()) != kOperationMagic) {
    return tl::make_unexpected(
        Status::NotSupported("Unknown WAL operation version"));
  }
  WalOperation op;
  op.epoch = decoder.u64();
  op.sequence = decoder.u64();
  op.kind = static_cast<Kind>(decoder.u32());
  const auto identity = decoder.take(op.schema_identity.size());
  std::copy(identity.begin(), identity.end(), op.schema_identity.begin());
  op.old_id = decoder.u64();
  op.new_id = decoder.u64();
  op.parent_row = decoder.u32();
  op.payload = decoder.take(decoder.u32());
  if (!decoder.done() || !Valid(op)) {
    return tl::make_unexpected(
        Status::InternalError("Invalid WAL operation envelope"));
  }
  return op;
}

WalFilePtr WalFile::Create(const std::string &wal_path) {
  return std::make_shared<LocalWalFile>(wal_path);
}

int WalFile::CreateAndOpen(const std::string &wal_path,
                           const WalOptions &wal_options,
                           WalFilePtr *wal_file) {
  *wal_file = std::make_shared<LocalWalFile>(wal_path);

  return (*wal_file)->open(wal_options);
}


};  // namespace zvec
