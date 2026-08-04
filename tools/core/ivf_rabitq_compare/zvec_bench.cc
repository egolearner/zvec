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

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <zvec/ailego/container/params.h>
#include <zvec/core/framework/index_factory.h>
#include <zvec/core/framework/index_holder.h>
#include <zvec/core/framework/index_meta.h>
#include "bench_common.h"
#include "ivf_rabitq_builder.h"
#include "ivf_rabitq_params.h"
#include "ivf_rabitq_streamer.h"
#include "rabitq_params.h"

namespace {

class VectorHolder final : public zvec::core::IndexHolder {
 public:
  class VectorIterator final : public zvec::core::IndexHolder::Iterator {
   public:
    VectorIterator(const std::vector<float> &values, size_t count, size_t dim)
        : values_(values), count_(count), dim_(dim) {}

    const void *data() const override {
      return values_.data() + offset_ * dim_;
    }

    bool is_valid() const override {
      return offset_ < count_;
    }

    uint64_t key() const override {
      return offset_;
    }

    void next() override {
      ++offset_;
    }

   private:
    const std::vector<float> &values_;
    size_t count_;
    size_t dim_;
    size_t offset_{0};
  };

  VectorHolder(const std::vector<float> &values, size_t count, size_t dim)
      : values_(values), count_(count), dim_(dim) {}

  size_t count() const override {
    return count_;
  }

  size_t dimension() const override {
    return dim_;
  }

  zvec::core::IndexMeta::DataType data_type() const override {
    return zvec::core::IndexMeta::DataType::DT_FP32;
  }

  size_t element_size() const override {
    return dim_ * sizeof(float);
  }

  bool multipass() const override {
    return true;
  }

  Iterator::Pointer create_iterator() override {
    return std::make_unique<VectorIterator>(values_, count_, dim_);
  }

 private:
  const std::vector<float> &values_;
  size_t count_;
  size_t dim_;
};

bool Check(int code, const char *operation, std::string *error) {
  if (code != 0) {
    return ivf_rabitq_bench::SetError(
        std::string(operation) + " failed, code=" + std::to_string(code),
        error);
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  using namespace ivf_rabitq_bench;
  using namespace zvec::core;
  Args args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    return Fail(error);
  }
  if (args.niters != 20) {
    return Fail(
        "Zvec IVF-RaBitQ training currently fixes OptKMeans at 20 "
        "iterations; set --niters to 20 for a fair build");
  }
  Vecs<float> base;
  Vecs<float> queries;
  Vecs<int32_t> gt;
  if (!LoadVecs(args.base, args.max_base, &base, &error) ||
      !LoadVecs(args.query, args.max_queries, &queries, &error) ||
      !LoadVecs(args.groundtruth, queries.count, &gt, &error)) {
    return Fail(error);
  }
  if (base.dim != queries.dim || gt.count < queries.count ||
      args.topk > gt.dim) {
    return Fail("dataset dimensions/counts do not match");
  }
  auto holder =
      std::make_shared<VectorHolder>(base.values, base.count, base.dim);
  IndexMeta meta(IndexMeta::DataType::DT_FP32, static_cast<uint32_t>(base.dim));
  meta.set_metric("SquaredEuclidean", 0, zvec::ailego::Params());
  zvec::ailego::Params params;
  params.set(PARAM_IVF_RABITQ_NLIST, static_cast<uint32_t>(args.nlist));
  params.set(PARAM_RABITQ_TOTAL_BITS, static_cast<uint32_t>(args.total_bits));
  params.set(PARAM_IVF_RABITQ_BUILDER_THREAD_COUNT,
             static_cast<uint32_t>(args.threads));
  size_t train_size = args.train_size == 0
                          ? std::min(base.count, args.nlist * 256)
                          : std::min(base.count, args.train_size);
  auto train_holder =
      std::make_shared<VectorHolder>(base.values, train_size, base.dim);
  params.set(PARAM_IVF_RABITQ_BRUTE_FORCE_THRESHOLD, 0U);

  auto builder = std::make_shared<IvfRabitqBuilder>();
  if (!Check(builder->init(meta, params), "zvec init", &error)) {
    return Fail(error);
  }
  auto train_begin = std::chrono::steady_clock::now();
  if (!Check(builder->train(nullptr, train_holder), "zvec train", &error)) {
    return Fail(error);
  }
  auto train_end = std::chrono::steady_clock::now();
  auto encode_begin = train_end;
  if (!Check(builder->build(nullptr, holder), "zvec build", &error)) {
    return Fail(error);
  }
  auto encode_end = std::chrono::steady_clock::now();

  std::string index_path = args.index_path;
  auto dumper = IndexFactory::CreateDumper("FileDumper");
  if (dumper == nullptr) {
    return Fail("cannot create zvec FileDumper");
  }
  if (!Check(dumper->create(index_path), "zvec dumper create", &error)) {
    return Fail(error);
  }
  auto dump_begin = std::chrono::steady_clock::now();
  if (!Check(builder->dump(dumper), "zvec dump", &error) ||
      !Check(dumper->close(), "zvec dumper close", &error)) {
    return Fail(error);
  }
  auto dump_end = std::chrono::steady_clock::now();
  PrintBuild("zvec", base.count, base.dim, args.threads,
             Seconds(train_begin, train_end), 0,
             Seconds(encode_begin, encode_end), Seconds(dump_begin, dump_end));
  builder.reset();

  auto storage = IndexFactory::CreateStorage("MMapFileReadStorage");
  if (storage == nullptr) {
    return Fail("cannot create zvec MMapFileReadStorage");
  }
  zvec::ailego::Params storage_params;
  storage_params.set("proxima.mmap_file.container.memory_warmup", true);
  if (!Check(storage->init(storage_params), "zvec storage init", &error) ||
      !Check(storage->open(index_path, false), "zvec storage open", &error)) {
    return Fail(error);
  }
  auto streamer = std::make_shared<IvfRabitqStreamer>();
  if (!Check(streamer->init(meta, params), "zvec streamer init", &error) ||
      !Check(streamer->open(storage), "zvec streamer open", &error)) {
    return Fail(error);
  }
  IndexQueryMeta query_meta(IndexMeta::DataType::DT_FP32,
                            static_cast<uint32_t>(queries.dim));
  std::vector<uint64_t> actual(queries.count * args.topk);
  for (size_t sthreads : args.search_threads) {
    for (size_t nprobe : args.nprobes) {
      std::vector<IndexStreamer::Context::Pointer> contexts(sthreads);
      for (size_t t = 0; t < sthreads; ++t) {
        auto context = streamer->create_context();
        context->set_topk(static_cast<uint32_t>(args.topk));
        zvec::ailego::Params search_params;
        search_params.set(PARAM_IVF_RABITQ_NPROBE,
                          static_cast<uint32_t>(nprobe));
        search_params.set(PARAM_IVF_RABITQ_BRUTE_FORCE_THRESHOLD, 0U);
        if (!Check(context->update(search_params), "zvec context update",
                   &error)) {
          return Fail(error);
        }
        contexts[t] = std::move(context);
      }

      size_t warmup = std::min(args.warmup, queries.count);
      for (size_t t = 0; t < sthreads; ++t) {
        for (size_t i = 0; i < warmup; ++i) {
          if (!Check(
                  streamer->search_impl(queries.values.data() + i * queries.dim,
                                        query_meta, contexts[t]),
                  "zvec warmup search", &error)) {
            return Fail(error);
          }
        }
      }

      std::atomic<bool> ok{true};
      auto worker = [&](size_t t, size_t begin, size_t end) {
        auto &context = contexts[t];
        for (size_t i = begin; i < end; ++i) {
          std::string local_error;
          if (!Check(
                  streamer->search_impl(queries.values.data() + i * queries.dim,
                                        query_meta, context),
                  "zvec search", &local_error)) {
            ok.store(false);
            return;
          }
          const auto &result = context->result();
          for (size_t j = 0; j < args.topk; ++j) {
            actual[i * args.topk + j] =
                j < result.size() ? result[j].key()
                                  : std::numeric_limits<uint64_t>::max();
          }
        }
      };

      double best = std::numeric_limits<double>::max();
      for (size_t repeat = 0; repeat < args.repeats; ++repeat) {
        auto begin = std::chrono::steady_clock::now();
        if (sthreads == 1) {
          worker(0, 0, queries.count);
        } else {
          std::vector<std::thread> pool;
          pool.reserve(sthreads);
          const size_t chunk = (queries.count + sthreads - 1) / sthreads;
          for (size_t t = 0; t < sthreads; ++t) {
            size_t b = std::min(t * chunk, queries.count);
            size_t e = std::min(b + chunk, queries.count);
            pool.emplace_back(worker, t, b, e);
          }
          for (auto &th : pool) {
            th.join();
          }
        }
        auto end = std::chrono::steady_clock::now();
        if (!ok.load()) {
          return Fail("zvec search");
        }
        best = std::min(best, Seconds(begin, end));
      }
      double recall = args.max_base == 0
                          ? RecallAtK(actual, gt, queries.count, args.topk)
                          : -1;
      PrintSearch("zvec", nprobe, queries.count, args.topk, sthreads, best,
                  recall);
    }
  }
  return 0;
}
