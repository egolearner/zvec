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

#include <omp.h>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <faiss/IndexIVF.h>
#include <faiss/IndexIVFRaBitQFastScan.h>
#include <faiss/IndexPreTransform.h>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#include "bench_common.h"

namespace {

faiss::IndexIVFRaBitQFastScan *GetIvf(faiss::Index *index) {
  auto *pre = dynamic_cast<faiss::IndexPreTransform *>(index);
  faiss::Index *inner = pre == nullptr ? index : pre->index;
  return dynamic_cast<faiss::IndexIVFRaBitQFastScan *>(inner);
}

}  // namespace

int main(int argc, char **argv) {
  using namespace ivf_rabitq_bench;
  Args args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    return Fail(error);
  }
  Vecs<float> base;
  Vecs<float> queries;
  Vecs<int32_t> gt;
  if (!LoadDataset(args, &base, &queries, &gt, &error)) {
    return Fail(error);
  }
  if (base.dim != queries.dim || gt.count < queries.count ||
      args.topk > gt.dim) {
    return Fail("dataset dimensions/counts do not match");
  }
  omp_set_num_threads(static_cast<int>(args.threads));
  std::string factory = "HR,IVF" + std::to_string(args.nlist) + ",RaBitQfs" +
                        std::to_string(args.total_bits);
  faiss::MetricType metric = args.metric == Metric::kCosine
                                 ? faiss::METRIC_INNER_PRODUCT
                                 : faiss::METRIC_L2;
  std::unique_ptr<faiss::Index> index(faiss::index_factory(
      static_cast<int>(base.dim), factory.c_str(), metric));
  faiss::IndexIVFRaBitQFastScan *ivf = GetIvf(index.get());
  if (ivf == nullptr) {
    return Fail("factory did not create IndexIVFRaBitQFastScan");
  }
  ivf->cp.niter = static_cast<int>(args.niters);
  ivf->cp.seed = 12345;
  ivf->qb = 8;
  size_t train_size = args.train_size == 0
                          ? std::min(base.count, args.nlist * 256)
                          : std::min(base.count, args.train_size);

  auto train_begin = std::chrono::steady_clock::now();
  index->train(static_cast<faiss::idx_t>(train_size), base.values.data());
  auto train_end = std::chrono::steady_clock::now();
  auto add_begin = train_end;
  index->add(static_cast<faiss::idx_t>(base.count), base.values.data());
  auto add_end = std::chrono::steady_clock::now();
  auto dump_begin = std::chrono::steady_clock::now();
  faiss::write_index(index.get(), args.index_path.c_str());
  auto dump_end = std::chrono::steady_clock::now();
  PrintBuild("faiss", base.count, base.dim, MetricName(args.metric),
             args.threads, Seconds(train_begin, train_end), 0,
             Seconds(add_begin, add_end), Seconds(dump_begin, dump_end));

  index.reset(faiss::read_index(args.index_path.c_str()));
  ivf = GetIvf(index.get());
  if (ivf == nullptr) {
    return Fail("reloaded index is not IndexIVFRaBitQFastScan");
  }

  omp_set_num_threads(1);
  std::vector<faiss::idx_t> labels(queries.count * args.topk);
  std::vector<float> distances(queries.count * args.topk);
  std::vector<uint64_t> actual(queries.count * args.topk);
  const std::vector<size_t> &sthreads_list = args.search_threads;
  for (size_t sthreads : sthreads_list) {
    for (size_t nprobe : args.nprobes) {
      ivf->nprobe = nprobe;
      size_t warmup = std::min(args.warmup, queries.count);
      for (size_t i = 0; i < warmup; ++i) {
        index->search(1, queries.values.data() + i * queries.dim,
                      static_cast<faiss::idx_t>(args.topk), distances.data(),
                      labels.data());
      }

      auto worker = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
          index->search(1, queries.values.data() + i * queries.dim,
                        static_cast<faiss::idx_t>(args.topk),
                        distances.data() + i * args.topk,
                        labels.data() + i * args.topk);
        }
      };

      double best = std::numeric_limits<double>::max();
      for (size_t repeat = 0; repeat < args.repeats; ++repeat) {
        auto begin = std::chrono::steady_clock::now();
        if (sthreads == 1) {
          worker(0, queries.count);
        } else {
          std::vector<std::thread> pool;
          pool.reserve(sthreads);
          const size_t chunk = (queries.count + sthreads - 1) / sthreads;
          for (size_t t = 0; t < sthreads; ++t) {
            size_t b = std::min(t * chunk, queries.count);
            size_t e = std::min(b + chunk, queries.count);
            pool.emplace_back(worker, b, e);
          }
          for (auto &th : pool) {
            th.join();
          }
        }
        auto end = std::chrono::steady_clock::now();
        best = std::min(best, Seconds(begin, end));
      }
      std::transform(labels.begin(), labels.end(), actual.begin(),
                     [&](faiss::idx_t id) {
                       return id < 0 ? std::numeric_limits<uint64_t>::max()
                                     : KeyAt(base, static_cast<size_t>(id));
                     });
      double recall = args.max_base == 0
                          ? RecallAtK(actual, gt, queries.count, args.topk)
                          : -1;
      PrintSearch("faiss", nprobe, queries.count, args.topk,
                  MetricName(args.metric), sthreads, best, recall);
    }
  }
  return 0;
}
