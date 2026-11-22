// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <zvec/db/status.h>

namespace zvec::fts {

// Owned by the collection. Leases follow the real arena/source lifetime,
// including a frozen arena held by a reader after a run has been published.
class FtsWriteBudget : public std::enable_shared_from_this<FtsWriteBudget> {
 public:
  enum class Part : size_t {
    Active,
    Frozen,
    Prepare,
    Metadata,
    Flush,
    Merge,
    Count
  };
  static constexpr size_t kParts = static_cast<size_t>(Part::Count);
  struct Usage {
    std::array<size_t, kParts> current{}, peak{};
    size_t peak_total{0};
    size_t peak_prepare_vectors{0};
    size_t peak_active_used{0}, peak_frozen_used{0};
  };
  class Lease {
   public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept {
      swap(other);
    }
    Lease &operator=(Lease &&other) noexcept {
      Lease old(std::move(other));
      swap(old);
      return *this;
    }
    ~Lease() {
      if (budget_) {
        budget_->release(part_, bytes_);
      }
    }
    Status freeze() {
      if (!budget_) {
        return Status::OK();
      }
      std::lock_guard lock(budget_->mutex_);
      auto &used = budget_->usage_.current;
      auto active = static_cast<size_t>(Part::Active);
      auto frozen = static_cast<size_t>(Part::Frozen);
      if (part_ != Part::Active || used[frozen] != 0) {
        return {StatusCode::RESOURCE_EXHAUSTED,
                "Collection frozen FTS arena is pinned"};
      }
      used[active] -= bytes_;
      used[frozen] += bytes_;
      part_ = Part::Frozen;
      budget_->record_peak(frozen);
      return Status::OK();
    }

   private:
    friend class FtsWriteBudget;
    Lease(std::shared_ptr<FtsWriteBudget> budget, Part part, size_t bytes)
        : budget_(std::move(budget)), part_(part), bytes_(bytes) {}
    void swap(Lease &other) noexcept {
      std::swap(budget_, other.budget_);
      std::swap(part_, other.part_);
      std::swap(bytes_, other.bytes_);
    }
    std::shared_ptr<FtsWriteBudget> budget_;
    Part part_{Part::Metadata};
    size_t bytes_{0};
  };

  FtsWriteBudget(size_t arena_bytes = 64u << 20,
                 size_t prepare_bytes = 32u << 20)
      : limits_{arena_bytes, arena_bytes, prepare_bytes,
                16u << 20,   32u << 20,   48u << 20} {}
  size_t limit(Part part) const {
    return limits_[static_cast<size_t>(part)];
  }
  Result<Lease> reserve_job(Part part) {
    if (part != Part::Flush && part != Part::Merge) {
      return tl::make_unexpected(
          Status::InvalidArgument("Invalid FTS write job"));
    }
    return reserve(part, limits_[static_cast<size_t>(part)]);
  }
  Result<Lease> reserve(Part part, size_t bytes) {
    std::lock_guard lock(mutex_);
    const size_t i = static_cast<size_t>(part);
    const size_t used = usage_.current[i];
    if (bytes > limits_[i] - used || (part != Part::Metadata && used != 0)) {
      return tl::make_unexpected(
          Status{StatusCode::RESOURCE_EXHAUSTED,
                 "Collection FTS writing budget exhausted"});
    }
    usage_.current[i] += bytes;
    record_peak(i);
    return Lease(shared_from_this(), part, bytes);
  }
  void observe_prepare(size_t vector_bytes, size_t arena_bytes) {
    std::lock_guard lock(mutex_);
    usage_.peak_prepare_vectors =
        std::max(usage_.peak_prepare_vectors, vector_bytes);
    usage_.peak_active_used = std::max(usage_.peak_active_used, arena_bytes);
  }
  void observe_frozen(size_t arena_bytes) {
    std::lock_guard lock(mutex_);
    usage_.peak_frozen_used = std::max(usage_.peak_frozen_used, arena_bytes);
  }
  Usage usage() const {
    std::lock_guard lock(mutex_);
    return usage_;
  }

 private:
  void record_peak(size_t i) {
    usage_.peak[i] = std::max(usage_.peak[i], usage_.current[i]);
    size_t sum = 0;
    for (auto n : usage_.current) {
      sum += n;
    }
    usage_.peak_total = std::max(usage_.peak_total, sum);
  }
  void release(Part part, size_t bytes) {
    std::lock_guard lock(mutex_);
    usage_.current[static_cast<size_t>(part)] -= bytes;
  }
  const std::array<size_t, kParts> limits_;
  mutable std::mutex mutex_;
  Usage usage_;
};

}  // namespace zvec::fts
