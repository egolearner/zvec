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

#include <charconv>
#include <filesystem>
#include <set>
#include <string_view>
#include "db/common/file_helper.h"
#include "db/index/column/fts_column/fts_file_lifetime.h"
#include "version_manager.h"

namespace zvec {
namespace {

bool Number(std::string_view text, uint64_t maximum) {
  if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0')) {
    return false;
  }
  uint64_t value = 0;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         value <= maximum;
}

bool StripSuffix(std::string_view *text, std::string_view suffix) {
  if (text->size() < suffix.size() ||
      text->substr(text->size() - suffix.size()) != suffix) {
    return false;
  }
  text->remove_suffix(suffix.size());
  return true;
}

bool OwnedRootFile(std::string_view name) {
  if (name == "manifest.current.pending") {
    return true;
  }
  if (name.substr(0, 9) == "manifest.") {
    name.remove_prefix(9);
    StripSuffix(&name, ".pending");
    return Number(name, UINT32_MAX);
  }
  return name.substr(0, 4) == "del." && Number(name.substr(4), UINT32_MAX);
}

bool OwnedFtsEntry(std::string_view name, bool directory) {
  if (!directory && name.substr(0, 10) == "wal.epoch.") {
    return name.substr(10) != "0" && Number(name.substr(10), UINT64_MAX);
  }
  if (name.substr(0, 4) != "fts.") {
    return false;
  }
  name.remove_prefix(4);
  auto separator = name.find('.');
  if (separator == std::string_view::npos ||
      !Number(name.substr(0, separator), UINT32_MAX)) {
    return false;
  }
  name.remove_prefix(separator + 1);
  if (directory && (name == "rocksdb" || name == "rocksdb.build")) {
    return true;
  }
  if (directory) {
    if (!StripSuffix(&name, ".reserved")) {
      return false;
    }
  } else {
    StripSuffix(&name, ".pending");
  }
  if (!StripSuffix(&name, ".zfts") && !StripSuffix(&name, ".run")) {
    return false;
  }
  separator = name.find('.');
  if (separator == std::string_view::npos ||
      !Number(name.substr(0, separator), UINT64_MAX) ||
      name.substr(0, separator) == "0") {
    return false;
  }
  name.remove_prefix(separator + 1);
  if (name.size() != 32) {
    return false;
  }
  for (const char c : name) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

struct Orphan {
  std::string path;
  bool directory;
  uint64_t bytes;
};

// Only regular files beneath an owned directory contribute to its charge.
// Recovery never follows links outside the retired tree.
uint64_t DirectoryBytes(const std::filesystem::path &directory,
                        std::error_code &error) {
  uint64_t total = 0;
  for (std::filesystem::recursive_directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    const auto type = it->symlink_status(error);
    if (error) {
      break;
    }
    if (!std::filesystem::is_regular_file(type)) {
      continue;
    }
    const auto bytes = it->file_size(error);
    if (error) {
      break;
    }
    if (bytes > UINT64_MAX - total) {
      error = std::make_error_code(std::errc::value_too_large);
      break;
    }
    total += bytes;
  }
  return total;
}

Status Collect(const std::string &directory, const std::set<std::string> &keep,
               bool root, std::vector<Orphan> *orphans) {
  std::error_code error;
  std::filesystem::directory_iterator it(
      ailego::FileHelper::PathFromUtf8(directory), error),
      end;
  // Bound one recovery sweep; further leftovers can be collected next open.
  while (!error && it != end && orphans->size() < 4096) {
    const auto status = it->symlink_status(error);
    if (error) {
      break;
    }
    const auto name = it->path().filename().u8string();
    const bool is_directory = std::filesystem::is_directory(status);
    if (!keep.count(name) && !std::filesystem::is_symlink(status) &&
        (is_directory || std::filesystem::is_regular_file(status)) &&
        (root ? (!is_directory && OwnedRootFile(name))
              : OwnedFtsEntry(name, is_directory))) {
      const auto bytes = is_directory ? DirectoryBytes(it->path(), error)
                                      : it->file_size(error);
      if (error) {
        break;
      }
      orphans->push_back({it->path().u8string(), is_directory, bytes});
    }
    it.increment(error);
  }
  return error ? Status::InternalError("Cannot enumerate recovery files: ",
                                       error.message())
               : Status::OK();
}

}  // namespace

Status VersionManager::reclaim_unreferenced_files() {
  std::lock_guard lock(mtx_);
  if (requires_recovery_ || !committed_id_) {
    return Status::FailedPrecondition("Recovery authority is not settled");
  }
  if (!explicit_authority_) {
    return Status::OK();
  }
  std::vector<Orphan> orphans;
  std::set<std::string> root_keep{
      "manifest." + std::to_string(*committed_id_),
      "del." + std::to_string(current_version_.delete_snapshot_path_suffix())};
  auto status = Collect(path_, root_keep, true, &orphans);
  if (!status.ok()) {
    return status;
  }
  auto metas = current_version_.persisted_segment_metas();
  if (current_version_.writing_segment_meta()) {
    metas.push_back(current_version_.writing_segment_meta());
  }
  for (const auto &meta : metas) {
    if (orphans.size() == 4096) {
      break;
    }
    const auto directory = FileHelper::MakeSegmentPath(path_, meta->id());
    std::set<std::string> keep;
    for (const auto &block : meta->persisted_blocks()) {
      if (block.type() != BlockType::FTS_INDEX) {
        continue;
      }
      if (block.fts_writing_) {
        const auto &root = *block.fts_writing_;
        keep.insert("wal.epoch." + std::to_string(root.wal_epoch));
        for (const auto &run : root.runs) {
          auto path = FileHelper::MakeFtsRunPath(directory, block.id(),
                                                 run.generation, run.id);
          keep.insert(
              ailego::FileHelper::PathFromUtf8(path).filename().u8string());
        }
        continue;
      }
      auto file = block.fts_base_
                      ? FileHelper::MakeCustomFtsPath(
                            directory, block.id(), block.fts_base_->generation,
                            block.fts_base_->base_id)
                      : FileHelper::MakeFtsIndexPath(directory, block.id());
      keep.insert(ailego::FileHelper::PathFromUtf8(file).filename().u8string());
    }
    status = Collect(directory, keep, false, &orphans);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto &orphan : orphans) {
    auto owner = fts_retirement_->track(orphan.path, orphan.directory, true);
    owner->register_bytes(orphan.bytes);
    owner->retire();
  }
  // Failure to remove is deferred by the queue and cannot roll back recovery.
  fts_retirement_->retry();
  return Status::OK();
}

}  // namespace zvec
