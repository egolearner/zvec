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
#include <thread>
#include <vector>
#include <faiss/Clustering.h>
#include <faiss/IndexFlat.h>
#include <rabitqlib/index/ivf/ivf.hpp>
#include "bench_common.h"

namespace {

constexpr bool kUseFasterConstruction = true;
constexpr bool kUseHighAccuracySearch = true;

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
  if (!LoadVecs(args.base, args.max_base, &base, &error) ||
      !LoadVecs(args.query, args.max_queries, &queries, &error) ||
      !LoadVecs(args.groundtruth, queries.count, &gt, &error)) {
    return Fail(error);
  }
  if (base.dim != queries.dim || gt.count < queries.count ||
      args.topk > gt.dim) {
    return Fail("dataset dimensions/counts do not match");
  }
  omp_set_num_threads(static_cast<int>(args.threads));
  size_t train_size = args.train_size == 0
                          ? std::min(base.count, args.nlist * 256)
                          : std::min(base.count, args.train_size);
  faiss::Clustering clustering(static_cast<int>(base.dim),
                               static_cast<int>(args.nlist));
  clustering.niter = static_cast<int>(args.niters);
  clustering.seed = 12345;
  clustering.verbose = false;
  faiss::IndexFlatL2 coarse(static_cast<faiss::idx_t>(base.dim));
  auto train_begin = std::chrono::steady_clock::now();
  clustering.train(static_cast<faiss::idx_t>(train_size), base.values.data(),
                   coarse);
  auto train_end = std::chrono::steady_clock::now();

  std::vector<faiss::idx_t> assignment64(base.count);
  std::vector<float> coarse_distances(base.count);
  auto assign_begin = std::chrono::steady_clock::now();
  coarse.search(static_cast<faiss::idx_t>(base.count), base.values.data(), 1,
                coarse_distances.data(), assignment64.data());
  auto assign_end = std::chrono::steady_clock::now();
  std::vector<rabitqlib::PID> assignments(base.count);
  std::transform(
      assignment64.begin(), assignment64.end(), assignments.begin(),
      [](faiss::idx_t id) { return static_cast<rabitqlib::PID>(id); });

  auto index = std::make_unique<rabitqlib::ivf::IVF>(
      base.count, base.dim, args.nlist, args.total_bits, rabitqlib::METRIC_L2,
      rabitqlib::RotatorType::FhtKacRotator);
  auto encode_begin = std::chrono::steady_clock::now();
  index->construct(base.values.data(), clustering.centroids.data(),
                   assignments.data(), kUseFasterConstruction);
  auto encode_end = std::chrono::steady_clock::now();
  auto dump_begin = std::chrono::steady_clock::now();
  index->save(args.index_path.c_str());
  auto dump_end = std::chrono::steady_clock::now();
  PrintBuild("rabitq_library", base.count, base.dim, args.threads,
             Seconds(train_begin, train_end), Seconds(assign_begin, assign_end),
             Seconds(encode_begin, encode_end), Seconds(dump_begin, dump_end));

  index.reset();
  auto search_index = std::make_unique<rabitqlib::ivf::IVF>();
  search_index->load(args.index_path.c_str());

  omp_set_num_threads(1);
  std::vector<uint64_t> actual(queries.count * args.topk);
  for (size_t sthreads : args.search_threads) {
    for (size_t nprobe : args.nprobes) {
      size_t warmup = std::min(args.warmup, queries.count);
      {
        std::vector<rabitqlib::PID> wlabels(args.topk);
        for (size_t i = 0; i < warmup; ++i) {
          search_index->search(queries.values.data() + i * queries.dim,
                               args.topk, nprobe, wlabels.data(),
                               kUseHighAccuracySearch);
        }
      }

      auto worker = [&](size_t begin, size_t end) {
        std::vector<rabitqlib::PID> labels(args.topk);
        for (size_t i = begin; i < end; ++i) {
          search_index->search(queries.values.data() + i * queries.dim,
                               args.topk, nprobe, labels.data(),
                               kUseHighAccuracySearch);
          std::copy(labels.begin(), labels.end(),
                    actual.begin() + i * args.topk);
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
      double recall = args.max_base == 0
                          ? RecallAtK(actual, gt, queries.count, args.topk)
                          : -1;
      PrintSearch("rabitq_library", nprobe, queries.count, args.topk, sthreads,
                  best, recall);
    }
  }
  return 0;
}
