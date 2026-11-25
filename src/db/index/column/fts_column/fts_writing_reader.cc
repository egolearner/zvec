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

#include "fts_writing_reader.h"
#include <algorithm>
#include <limits>

namespace zvec::fts {
namespace {

constexpr float kUnbounded = std::numeric_limits<float>::infinity();

struct TermPart {
  uint32_t begin;
  uint32_t end;
  FtsReadTerm::Ptr term;
};
using Parts = std::vector<TermPart>;

class JoinedPosting final : public FtsReadPosting {
 public:
  JoinedPosting(std::shared_ptr<const Parts> parts, std::shared_ptr<void> lease)
      : parts_(std::move(parts)), lease_(std::move(lease)) {}

  Result<uint32_t> next() override {
    if (!status_.ok()) {
      return tl::make_unexpected(status_);
    }
    if (started_ && doc_ == UINT32_MAX) {
      return doc_;
    }
    return seek(started_ ? doc_ + 1 : 0, true);
  }

  Result<uint32_t> advance(uint32_t target) override {
    if (!status_.ok()) {
      return tl::make_unexpected(status_);
    }
    if (started_ && doc_ >= target) {
      return doc_;
    }
    return seek(target, false);
  }

  Result<FtsReadPayload> payload() override {
    if (!status_.ok()) {
      return tl::make_unexpected(status_);
    }
    if (!cursor_) {
      return tl::make_unexpected(
          Status::FailedPrecondition("Writing posting has no current row"));
    }
    auto result = cursor_->payload();
    if (!result) {
      status_ = result.error();
    }
    return result;
  }

  Result<FtsReadPosition::Ptr> current_positions(const FtsReadTerm &,
                                                 uint32_t row,
                                                 bool retain_windows) override {
    if (!status_.ok()) {
      return tl::make_unexpected(status_);
    }
    if (!cursor_ || row != doc_ || row == UINT32_MAX) {
      return tl::make_unexpected(Status::FailedPrecondition(
          "Writing posting is not at the requested row"));
    }
    const auto &part = (*parts_)[part_];
    return cursor_->current_positions(*part.term, row - part.begin,
                                      retain_windows);
  }

  Result<FtsPostingBatch> next_batch() override {
    if (!status_.ok()) {
      return tl::make_unexpected(status_);
    }
    for (; part_ < parts_->size(); ++part_) {
      const auto &part = (*parts_)[part_];
      auto status = open_cursor(part);
      if (!status.ok()) {
        return tl::make_unexpected(status_ = status);
      }
      auto batch = cursor_->next_batch();
      if (!batch) {
        return tl::make_unexpected(status_ = batch.error());
      }
      if (batch->count != 0) {
        if (batch->count > docs_.size() || !batch->docs || !batch->tfs ||
            !batch->lengths ||
            batch->docs[batch->count - 1] >= part.end - part.begin ||
            (started_ && part.begin + batch->docs[0] <= doc_)) {
          return tl::make_unexpected(
              status_ = Status::InternalError("Invalid writing posting batch"));
        }
        // Children yield ascending local IDs. Only the IDs need translation;
        // the child's payload arrays remain pinned until this cursor advances.
        for (uint32_t i = 0; i < batch->count; ++i) {
          docs_[i] = part.begin + batch->docs[i];
        }
        batch->docs = docs_.data();
        doc_ = docs_[batch->count - 1];
        started_ = true;
        return batch;
      }
      cursor_.reset();
    }
    started_ = true;
    doc_ = UINT32_MAX;
    return FtsPostingBatch{};
  }

  Result<Bound> bound(uint32_t, float) override {
    if (!status_.ok()) {
      return tl::make_unexpected(status_);
    }
    return Bound{kUnbounded, UINT32_MAX};
  }

 private:
  Status open_cursor(const TermPart &part) {
    if (!cursor_) {
      auto opened = part.term->postings();
      if (!opened) {
        return opened.error();
      }
      cursor_ = std::move(*opened);
    }
    return Status::OK();
  }

  Result<uint32_t> seek(uint32_t target, bool sequential) {
    started_ = true;
    for (; part_ < parts_->size(); ++part_) {
      const auto &part = (*parts_)[part_];
      if (target < part.end) {
        auto status = open_cursor(part);
        if (!status.ok()) {
          return tl::make_unexpected(status_ = status);
        }
        auto row =
            sequential
                ? cursor_->next()
                : cursor_->advance(std::max(target, part.begin) - part.begin);
        if (!row) {
          return tl::make_unexpected(status_ = row.error());
        }
        if (*row != UINT32_MAX) {
          if (*row >= part.end - part.begin || part.begin + *row < target) {
            return tl::make_unexpected(
                status_ = Status::InternalError(
                    "Writing posting outside its row domain"));
          }
          return doc_ = part.begin + *row;
        }
      }
      cursor_.reset();
    }
    return doc_ = UINT32_MAX;
  }

  std::shared_ptr<const Parts> parts_;
  std::shared_ptr<void> lease_;
  size_t part_{0};
  FtsReadPosting::Ptr cursor_;
  std::array<uint32_t, FtsPostingBatch::kCapacity> docs_;
  bool started_{false};
  Status status_;
  uint32_t doc_{UINT32_MAX};
};

class JoinedTerm final : public FtsReadTerm {
 public:
  JoinedTerm(Parts parts, uint64_t df, std::shared_ptr<FtsReadBudget> budget)
      : parts_(std::make_shared<const Parts>(std::move(parts))),
        df_(df),
        budget_(std::move(budget)) {}
  uint64_t df() const override {
    return df_;
  }
  float max_score(float) const override {
    return kUnbounded;
  }
  Result<FtsReadPosting::Ptr> postings() const override {
    auto lease = budget_->reserve(sizeof(JoinedPosting));
    if (!lease) {
      return tl::make_unexpected(lease.error());
    }
    return std::make_unique<JoinedPosting>(parts_, std::move(*lease));
  }
  Result<FtsReadPosition::Ptr> positions(uint32_t row) const override {
    auto part = std::lower_bound(
        parts_->begin(), parts_->end(), row,
        [](const TermPart &entry, uint32_t doc) { return entry.end <= doc; });
    if (part == parts_->end() || row < part->begin) {
      return tl::make_unexpected(
          Status::InternalError("No writing term at phrase document"));
    }
    return part->term->positions(row - part->begin);
  }

 private:
  std::shared_ptr<const Parts> parts_;
  uint64_t df_;
  std::shared_ptr<FtsReadBudget> budget_;
};

class JoinedReader final : public FtsSegmentReader {
 public:
  JoinedReader(std::vector<Ptr> inputs, std::vector<uint32_t> rows,
               BM25ScorerPtr scorer, std::shared_ptr<FtsReadBudget> budget)
      : inputs_(std::move(inputs)),
        rows_(std::move(rows)),
        scorer_(std::move(scorer)),
        budget_(std::move(budget)) {}
  BM25ScorerPtr scorer() const override {
    return scorer_;
  }
  bool supports_batches(bool) const override {
    // Child bounds do not apply to parent-segment BM25. BatchDisjunction
    // filters parent IDs before they reach TopK regardless of run boundaries.
    return std::all_of(inputs_.begin(), inputs_.end(), [](const auto &input) {
      return input->supports_batches();
    });
  }
  Result<FtsReadTerm::Ptr> lookup(std::string_view key) const override {
    Parts parts;
    uint64_t df = 0;
    uint32_t begin = 0;
    for (size_t i = 0; i < inputs_.size(); ++i) {
      auto term = inputs_[i]->lookup(key);
      if (!term) {
        return tl::make_unexpected(term.error());
      }
      if (*term) {
        if ((*term)->df() == 0 || (*term)->df() > rows_[i]) {
          return tl::make_unexpected(
              Status::InternalError("Invalid writing term frequency"));
        }
        df += (*term)->df();
        parts.push_back({begin, begin + rows_[i], std::move(*term)});
      }
      begin += rows_[i];
    }
    if (parts.empty()) {
      return FtsReadTerm::Ptr{};
    }
    return std::make_shared<JoinedTerm>(std::move(parts), df, budget_);
  }

 private:
  std::vector<Ptr> inputs_;
  std::vector<uint32_t> rows_;
  BM25ScorerPtr scorer_;
  std::shared_ptr<FtsReadBudget> budget_;
};

}  // namespace

Result<FtsSegmentReader::Ptr> OpenWritingFtsReader(
    std::vector<FtsSegmentReader::Ptr> inputs, std::vector<uint32_t> rows,
    std::shared_ptr<FtsReadBudget> budget) {
  // One active and one frozen buffer may join the bounded disk root.
  if (inputs.size() != rows.size() || inputs.size() > kMaxWritingRuns + 2) {
    return tl::make_unexpected(Status::InvalidArgument("Invalid writing root"));
  }
  BM25Params params;
  uint64_t frontier = 0, documents = 0, tokens = 0;
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (!inputs[i] || !inputs[i]->scorer() ||
        !format::ValidRange(frontier, rows[i], UINT32_MAX)) {
      return tl::make_unexpected(
          Status::InvalidArgument("Invalid writing input"));
    }
    const auto scorer = inputs[i]->scorer();
    const auto stats = scorer->stats();
    if (i == 0) {
      params = scorer->params();
    }
    if (scorer->params().k1 != params.k1 || scorer->params().b != params.b ||
        stats.total_docs > rows[i] ||
        !format::ValidRange(tokens, stats.total_tokens, UINT64_MAX)) {
      return tl::make_unexpected(
          Status::InvalidArgument("Incompatible writing statistics"));
    }
    documents += stats.total_docs;
    tokens += stats.total_tokens;
    frontier += rows[i];
  }
  auto scorer = std::make_shared<BM25Scorer>(params);
  scorer->update_stats(documents, tokens);
  return std::make_shared<JoinedReader>(
      std::move(inputs), std::move(rows), std::move(scorer),
      budget ? std::move(budget) : std::make_shared<FtsReadBudget>(64u << 20));
}

}  // namespace zvec::fts
