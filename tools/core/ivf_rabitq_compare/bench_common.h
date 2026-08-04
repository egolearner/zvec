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
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ivf_rabitq_bench {

enum class DatasetFormat {
  kStandardVecs,
  kCohere1M,
};

enum class Metric {
  kL2,
  kCosine,
};

struct Args {
  std::string base;
  std::string query;
  std::string groundtruth;
  std::string index_path;
  DatasetFormat dataset_format{DatasetFormat::kStandardVecs};
  Metric metric{Metric::kL2};
  size_t nlist{1024};
  size_t train_size{0};
  size_t max_base{0};
  size_t max_queries{0};
  size_t topk{10};
  size_t threads{std::max(std::thread::hardware_concurrency(), 1U)};
  std::vector<size_t> search_threads{1};
  size_t repeats{3};
  size_t warmup{20};
  size_t niters{20};
  size_t total_bits{1};
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
    } else if (name == "--dataset") {
      if (value != "cohere-1m") {
        return SetError("unsupported dataset: " + value, error);
      }
      parsed.dataset_format = DatasetFormat::kCohere1M;
    } else if (name == "--dataset-dir") {
      parsed.dataset_format = DatasetFormat::kCohere1M;
      parsed.base = (std::filesystem::path(value) /
                     "cohere_train_vector_1m.new.zvec.vecs")
                        .string();
      parsed.query =
          (std::filesystem::path(value) / "cohere_test_vector_1m.1000.new.txt")
              .string();
      parsed.groundtruth =
          (std::filesystem::path(value) / "neighbors.txt").string();
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
    } else if (name == "--search-threads") {
      if (!ParseSizes(value, &parsed.search_threads, error)) {
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
    } else if (name == "--total-bits") {
      if (!ParseSize(value, "total-bits", false, &parsed.total_bits, error)) {
        return false;
      }
      if (parsed.total_bits < 1 || parsed.total_bits > 9) {
        return SetError("total-bits must be in [1, 9]", error);
      }
    } else if (name == "--metric") {
      if (value == "l2") {
        parsed.metric = Metric::kL2;
      } else if (value == "cosine") {
        parsed.metric = Metric::kCosine;
      } else {
        return SetError("metric must be l2 or cosine", error);
      }
    } else if (name == "--nprobes") {
      if (!ParseSizes(value, &parsed.nprobes, error)) {
        return false;
      }
    } else {
      return SetError("unknown argument: " + name, error);
    }
  }
  if (parsed.dataset_format == DatasetFormat::kCohere1M) {
    parsed.metric = Metric::kCosine;
  }
  if (parsed.base.empty() || parsed.query.empty() ||
      parsed.groundtruth.empty() || parsed.index_path.empty()) {
    return SetError("--base, --query, --groundtruth and --index are required",
                    error);
  }
  *args = std::move(parsed);
  return true;
}

inline const char *MetricName(Metric metric) {
  return metric == Metric::kCosine ? "cosine" : "l2";
}

template <typename T>
struct Vecs {
  size_t count{0};
  size_t dim{0};
  std::vector<T> values;
  std::vector<uint64_t> keys;
};

template <typename T>
uint64_t KeyAt(const Vecs<T> &vecs, size_t index) {
  if (index >= vecs.count) {
    return std::numeric_limits<uint64_t>::max();
  }
  return vecs.keys.empty() ? index : vecs.keys[index];
}

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

#pragma pack(push, 4)
struct ZvecVecsHeader {
  uint64_t num_vecs;
  uint16_t meta_size_v1;
  uint16_t version;
  uint32_t meta_size;
  uint64_t bitmap;
  uint64_t key_offset;
  uint64_t key_size;
  uint64_t dense_offset;
  uint64_t dense_size;
  uint64_t sparse_offset;
  uint64_t sparse_size;
  uint64_t partition_offset;
  uint64_t partition_size;
  uint64_t taglist_offset;
  uint64_t taglist_size;
};

struct ZvecIndexMetaHeader {
  uint32_t header_size;
  uint32_t meta_type;
  uint32_t major_order;
  uint32_t data_type;
  uint32_t dimension;
  uint32_t unit_size;
};
#pragma pack(pop)

static_assert(sizeof(ZvecVecsHeader) == 104,
              "unexpected Zvec vecs header size");

inline bool LoadZvecFloatVecs(const std::string &path, size_t limit,
                              Vecs<float> *result, std::string *error) {
  if (result == nullptr) {
    return SetError("null Zvec vecs output", error);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return SetError("cannot open " + path, error);
  }
  ZvecVecsHeader header{};
  ZvecIndexMetaHeader meta{};
  input.read(reinterpret_cast<char *>(&header), sizeof(header));
  input.read(reinterpret_cast<char *>(&meta), sizeof(meta));
  constexpr uint32_t kFp32DataType = 2;
  if (!input || header.version != 1 ||
      header.meta_size < sizeof(ZvecIndexMetaHeader) ||
      meta.header_size > header.meta_size || meta.data_type != kFp32DataType ||
      meta.dimension == 0 || meta.unit_size != sizeof(float)) {
    return SetError("invalid FP32 Zvec vecs header in " + path, error);
  }

  if (header.num_vecs > std::numeric_limits<size_t>::max()) {
    return SetError("Zvec vecs count overflow: " + path, error);
  }
  size_t count = static_cast<size_t>(header.num_vecs);
  if (limit > 0) {
    count = std::min(count, limit);
  }
  if (count > std::numeric_limits<size_t>::max() / meta.dimension) {
    return SetError("Zvec vecs size overflow: " + path, error);
  }
  size_t value_count = count * static_cast<size_t>(meta.dimension);
  if (value_count > std::numeric_limits<size_t>::max() / sizeof(float)) {
    return SetError("Zvec vecs byte size overflow: " + path, error);
  }
  size_t data_offset = sizeof(ZvecVecsHeader) + header.meta_size;
  input.seekg(0, std::ios::end);
  std::streampos end_position = input.tellg();
  if (end_position < 0 || static_cast<size_t>(end_position) <
                              data_offset + value_count * sizeof(float)) {
    return SetError("short Zvec vecs file: " + path, error);
  }

  Vecs<float> loaded;
  loaded.count = count;
  loaded.dim = meta.dimension;
  loaded.values.resize(value_count);
  input.seekg(static_cast<std::streamoff>(data_offset));
  input.read(reinterpret_cast<char *>(loaded.values.data()),
             static_cast<std::streamsize>(value_count * sizeof(float)));
  if (!input) {
    return SetError("short read from " + path, error);
  }
  if (header.num_vecs >
          std::numeric_limits<uint64_t>::max() / sizeof(uint64_t) ||
      header.key_size < header.num_vecs * sizeof(uint64_t) ||
      header.key_offset > static_cast<uint64_t>(end_position) - data_offset ||
      count * sizeof(uint64_t) > static_cast<uint64_t>(end_position) -
                                     data_offset - header.key_offset) {
    return SetError("invalid key section in " + path, error);
  }
  loaded.keys.resize(count);
  input.seekg(static_cast<std::streamoff>(data_offset + header.key_offset));
  input.read(reinterpret_cast<char *>(loaded.keys.data()),
             static_cast<std::streamsize>(count * sizeof(uint64_t)));
  if (!input) {
    return SetError("short key read from " + path, error);
  }
  *result = std::move(loaded);
  return true;
}

template <typename T>
bool LoadCohereTextVecs(const std::string &path, size_t limit, Vecs<T> *result,
                        std::string *error) {
  if (result == nullptr) {
    return SetError("null Cohere text output", error);
  }
  std::ifstream input(path);
  if (!input) {
    return SetError("cannot open " + path, error);
  }
  Vecs<T> loaded;
  std::string line;
  while ((limit == 0 || loaded.count < limit) && std::getline(input, line)) {
    size_t first_separator = line.find(';');
    if (first_separator == std::string::npos) {
      return SetError("missing ';' in " + path, error);
    }
    size_t second_separator = line.find(';', first_separator + 1);
    std::string values_text = line.substr(
        first_separator + 1, second_separator - first_separator - 1);
    std::istringstream values_stream(values_text);
    std::vector<T> row;
    T value{};
    while (values_stream >> value) {
      row.push_back(value);
    }
    if (!values_stream.eof()) {
      return SetError("invalid text vector value in " + path, error);
    }
    if (row.empty()) {
      return SetError("empty vector in " + path, error);
    }
    if (loaded.dim == 0) {
      loaded.dim = row.size();
    } else if (loaded.dim != row.size()) {
      return SetError("inconsistent text vector dimension in " + path, error);
    }
    loaded.values.insert(loaded.values.end(), row.begin(), row.end());
    ++loaded.count;
  }
  if (!input.eof() && input.fail()) {
    return SetError("failed to read " + path, error);
  }
  *result = std::move(loaded);
  return true;
}

inline bool NormalizeRows(Vecs<float> *vectors, std::string *error) {
  if (vectors == nullptr) {
    return SetError("null vectors to normalize", error);
  }
  for (size_t i = 0; i < vectors->count; ++i) {
    float *row = vectors->values.data() + i * vectors->dim;
    double squared_norm = 0;
    for (size_t j = 0; j < vectors->dim; ++j) {
      squared_norm += static_cast<double>(row[j]) * row[j];
    }
    if (!(squared_norm > 0) || !std::isfinite(squared_norm)) {
      return SetError("cannot normalize zero or non-finite vector", error);
    }
    float inverse_norm = static_cast<float>(1.0 / std::sqrt(squared_norm));
    for (size_t j = 0; j < vectors->dim; ++j) {
      row[j] *= inverse_norm;
    }
  }
  return true;
}

inline bool LoadDataset(const Args &args, Vecs<float> *base,
                        Vecs<float> *queries, Vecs<int32_t> *groundtruth,
                        std::string *error) {
  bool loaded = false;
  if (args.dataset_format == DatasetFormat::kCohere1M) {
    loaded = LoadZvecFloatVecs(args.base, args.max_base, base, error) &&
             LoadCohereTextVecs(args.query, args.max_queries, queries, error) &&
             LoadCohereTextVecs(args.groundtruth, queries->count, groundtruth,
                                error);
  } else {
    loaded = LoadVecs(args.base, args.max_base, base, error) &&
             LoadVecs(args.query, args.max_queries, queries, error) &&
             LoadVecs(args.groundtruth, queries->count, groundtruth, error);
  }
  if (!loaded) {
    return false;
  }
  if (args.metric == Metric::kCosine) {
    return NormalizeRows(base, error) && NormalizeRows(queries, error);
  }
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
                       const char *metric, size_t threads, double train_seconds,
                       double assign_seconds, double encode_seconds,
                       double dump_seconds) {
  std::cout << std::fixed << std::setprecision(6)
            << "{\"type\":\"build\",\"engine\":\"" << engine
            << "\",\"count\":" << count << ",\"dim\":" << dim
            << ",\"metric\":\"" << metric << "\""
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
                        size_t topk, const char *metric, size_t search_threads,
                        double seconds, double recall) {
  std::cout << std::fixed << std::setprecision(6)
            << "{\"type\":\"search\",\"engine\":\"" << engine
            << "\",\"nprobe\":" << nprobe << ",\"queries\":" << query_count
            << ",\"topk\":" << topk << ",\"threads\":" << search_threads
            << ",\"metric\":\"" << metric << "\""
            << ",\"seconds\":" << seconds
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
