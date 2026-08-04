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

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ivf_rabitq_bench {

struct Args {
  std::string base;
  std::string query;
  std::string groundtruth;
  std::string index_path;
  size_t nlist{1024};
  size_t train_size{0};
  size_t max_base{0};
  size_t max_queries{0};
  size_t topk{10};
  size_t threads{std::max(std::thread::hardware_concurrency(), 1U)};
  size_t repeats{3};
  size_t warmup{20};
  size_t niters{20};
  std::vector<size_t> nprobes{1, 2, 4, 8, 16, 32, 64, 128, 256};
};

inline bool SetError(const std::string &message, std::string *error) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

inline int Fail(const std::string &message) {
  std::cerr << "error: " << message << std::endl;
  return 1;
}

inline bool ParseSize(const std::string &value, const char *name,
                      bool allow_zero, size_t *result, std::string *error) {
  if (result == nullptr) {
    return SetError("null size output", error);
  }
  size_t parsed = 0;
  auto parse_result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || parse_result.ec != std::errc() ||
      parse_result.ptr != value.data() + value.size() ||
      (!allow_zero && parsed == 0)) {
    return SetError(std::string("invalid ") + name + ": " + value, error);
  }
  *result = parsed;
  return true;
}

inline bool ParseSizes(const std::string &value, std::vector<size_t> *result,
                       std::string *error) {
  if (result == nullptr) {
    return SetError("null nprobes output", error);
  }
  std::vector<size_t> parsed;
  size_t begin = 0;
  while (begin < value.size()) {
    size_t end = value.find(',', begin);
    std::string item = value.substr(begin, end - begin);
    size_t nprobe = 0;
    if (!ParseSize(item, "nprobes", false, &nprobe, error)) {
      return false;
    }
    parsed.push_back(nprobe);
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  if (parsed.empty()) {
    return SetError("invalid nprobes: " + value, error);
  }
  std::sort(parsed.begin(), parsed.end());
  parsed.erase(std::unique(parsed.begin(), parsed.end()), parsed.end());
  *result = std::move(parsed);
  return true;
}

inline bool ParseArgs(int argc, char **argv, Args *args, std::string *error) {
  if (args == nullptr) {
    return SetError("null argument output", error);
  }
  Args parsed;
  for (int i = 1; i < argc; ++i) {
    std::string name(argv[i]);
    if (i + 1 >= argc) {
      return SetError("missing value for " + name, error);
    }
    std::string value(argv[++i]);
    if (name == "--base") {
      parsed.base = value;
    } else if (name == "--query") {
      parsed.query = value;
    } else if (name == "--groundtruth") {
      parsed.groundtruth = value;
    } else if (name == "--index") {
      parsed.index_path = value;
    } else if (name == "--nlist") {
      if (!ParseSize(value, "nlist", false, &parsed.nlist, error)) {
        return false;
      }
    } else if (name == "--train-size") {
      if (!ParseSize(value, "train-size", true, &parsed.train_size, error)) {
        return false;
      }
    } else if (name == "--max-base") {
      if (!ParseSize(value, "max-base", true, &parsed.max_base, error)) {
        return false;
      }
    } else if (name == "--max-queries") {
      if (!ParseSize(value, "max-queries", true, &parsed.max_queries, error)) {
        return false;
      }
    } else if (name == "--topk") {
      if (!ParseSize(value, "topk", false, &parsed.topk, error)) {
        return false;
      }
    } else if (name == "--threads") {
      if (!ParseSize(value, "threads", false, &parsed.threads, error)) {
        return false;
      }
    } else if (name == "--repeats") {
      if (!ParseSize(value, "repeats", false, &parsed.repeats, error)) {
        return false;
      }
    } else if (name == "--warmup") {
      if (!ParseSize(value, "warmup", true, &parsed.warmup, error)) {
        return false;
      }
    } else if (name == "--niters") {
      if (!ParseSize(value, "niters", false, &parsed.niters, error)) {
        return false;
      }
    } else if (name == "--nprobes") {
      if (!ParseSizes(value, &parsed.nprobes, error)) {
        return false;
      }
    } else {
      return SetError("unknown argument: " + name, error);
    }
  }
  if (parsed.base.empty() || parsed.query.empty() ||
      parsed.groundtruth.empty() || parsed.index_path.empty()) {
    return SetError("--base, --query, --groundtruth and --index are required",
                    error);
  }
  *args = std::move(parsed);
  return true;
}

template <typename T>
struct Vecs {
  size_t count{0};
  size_t dim{0};
  std::vector<T> values;
};

template <typename T>
bool LoadVecs(const std::string &path, size_t limit, Vecs<T> *result,
              std::string *error) {
  if (result == nullptr) {
    return SetError("null vecs output", error);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return SetError("cannot open " + path, error);
  }
  int32_t dim = 0;
  input.read(reinterpret_cast<char *>(&dim), sizeof(dim));
  if (!input || dim <= 0) {
    return SetError("invalid vecs header in " + path, error);
  }
  input.seekg(0, std::ios::end);
  std::streampos end_position = input.tellg();
  if (end_position < 0) {
    return SetError("cannot determine vecs file size: " + path, error);
  }
  size_t bytes = static_cast<size_t>(end_position);
  size_t row_bytes = sizeof(int32_t) + static_cast<size_t>(dim) * sizeof(T);
  if (bytes % row_bytes != 0) {
    return SetError("invalid vecs file size: " + path, error);
  }
  size_t count = bytes / row_bytes;
  if (limit > 0) {
    count = std::min(count, limit);
  }
  Vecs<T> loaded;
  loaded.count = count;
  loaded.dim = static_cast<size_t>(dim);
  loaded.values.resize(count * loaded.dim);
  input.seekg(0);
  for (size_t i = 0; i < count; ++i) {
    int32_t row_dim = 0;
    input.read(reinterpret_cast<char *>(&row_dim), sizeof(row_dim));
    if (row_dim != dim) {
      return SetError("inconsistent vecs dimension in " + path, error);
    }
    input.read(reinterpret_cast<char *>(loaded.values.data() + i * loaded.dim),
               static_cast<std::streamsize>(loaded.dim * sizeof(T)));
  }
  if (!input) {
    return SetError("short read from " + path, error);
  }
  *result = std::move(loaded);
  return true;
}

inline double Seconds(const std::chrono::steady_clock::time_point &begin,
                      const std::chrono::steady_clock::time_point &end) {
  return std::chrono::duration<double>(end - begin).count();
}

inline double RecallAtK(const std::vector<uint64_t> &actual,
                        const Vecs<int32_t> &groundtruth, size_t query_count,
                        size_t topk) {
  size_t hits = 0;
  for (size_t i = 0; i < query_count; ++i) {
    std::unordered_set<uint64_t> expected;
    for (size_t j = 0; j < std::min(topk, groundtruth.dim); ++j) {
      expected.insert(
          static_cast<uint64_t>(groundtruth.values[i * groundtruth.dim + j]));
    }
    for (size_t j = 0; j < topk; ++j) {
      if (expected.count(actual[i * topk + j]) != 0) {
        ++hits;
      }
    }
  }
  return static_cast<double>(hits) / static_cast<double>(query_count * topk);
}

inline void PrintBuild(const char *engine, size_t count, size_t dim,
                       size_t threads, double train_seconds,
                       double assign_seconds, double encode_seconds,
                       double dump_seconds) {
  std::cout << std::fixed << std::setprecision(6)
            << "{\"type\":\"build\",\"engine\":\"" << engine
            << "\",\"count\":" << count << ",\"dim\":" << dim
            << ",\"threads\":" << threads
            << ",\"train_seconds\":" << train_seconds
            << ",\"assign_seconds\":" << assign_seconds
            << ",\"encode_seconds\":" << encode_seconds << ",\"build_seconds\":"
            << train_seconds + assign_seconds + encode_seconds
            << ",\"dump_seconds\":" << dump_seconds << ",\"total_seconds\":"
            << train_seconds + assign_seconds + encode_seconds + dump_seconds
            << "}" << std::endl;
}

inline void PrintSearch(const char *engine, size_t nprobe, size_t query_count,
                        size_t topk, double seconds, double recall) {
  std::cout << std::fixed << std::setprecision(6)
            << "{\"type\":\"search\",\"engine\":\"" << engine
            << "\",\"nprobe\":" << nprobe << ",\"queries\":" << query_count
            << ",\"topk\":" << topk << ",\"threads\":1,\"seconds\":" << seconds
            << ",\"qps\":" << static_cast<double>(query_count) / seconds
            << ",\"recall_at_k\":";
  if (recall < 0) {
    std::cout << "null";
  } else {
    std::cout << recall;
  }
  std::cout << "}" << std::endl;
}

}  // namespace ivf_rabitq_bench
