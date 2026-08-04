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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "bench_common.h"

namespace ivf_rabitq_bench {
namespace {

std::vector<char *> MakeArgv(std::vector<std::string> *arguments) {
  std::vector<char *> argv;
  argv.reserve(arguments->size());
  for (auto &argument : *arguments) {
    argv.push_back(argument.data());
  }
  return argv;
}

std::filesystem::path TempVecsPath() {
  auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::path(testing::TempDir()) /
         ("ivf_rabitq_bench_" + std::to_string(suffix) + ".fvecs");
}

std::filesystem::path TempPath(const std::string &suffix) {
  auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::path(testing::TempDir()) /
         ("ivf_rabitq_bench_" + std::to_string(timestamp) + suffix);
}

TEST(IvfRabitqBenchCommonTest, ParseArgs) {
  std::vector<std::string> arguments{
      "bench",       "--base",        "base.fvecs", "--query",
      "query.fvecs", "--groundtruth", "gt.ivecs",   "--index",
      "index.bin",   "--nlist",       "64",         "--nprobes",
      "16,4,4,1",    "--threads",     "8",
  };
  std::vector<char *> argv = MakeArgv(&arguments);
  Args args;
  std::string error;
  ASSERT_TRUE(
      ParseArgs(static_cast<int>(argv.size()), argv.data(), &args, &error))
      << error;
  EXPECT_EQ(args.nlist, 64U);
  EXPECT_EQ(args.threads, 8U);
  EXPECT_EQ(args.nprobes, (std::vector<size_t>{1, 4, 16}));

  arguments = {"bench", "--nlist", "invalid"};
  argv = MakeArgv(&arguments);
  EXPECT_FALSE(
      ParseArgs(static_cast<int>(argv.size()), argv.data(), &args, &error));
  EXPECT_NE(error.find("invalid nlist"), std::string::npos);
}

TEST(IvfRabitqBenchCommonTest, ParseCohereDatasetArgs) {
  std::vector<std::string> arguments{
      "bench",        "--dataset", "cohere-1m", "--dataset-dir",
      "/data/cohere", "--index",   "index.bin",
  };
  std::vector<char *> argv = MakeArgv(&arguments);
  Args args;
  std::string error;
  ASSERT_TRUE(
      ParseArgs(static_cast<int>(argv.size()), argv.data(), &args, &error))
      << error;
  EXPECT_EQ(DatasetFormat::kCohere1M, args.dataset_format);
  EXPECT_EQ(Metric::kCosine, args.metric);
  EXPECT_EQ("/data/cohere/cohere_train_vector_1m.new.zvec.vecs", args.base);
  EXPECT_EQ("/data/cohere/cohere_test_vector_1m.1000.new.txt", args.query);
  EXPECT_EQ("/data/cohere/neighbors.txt", args.groundtruth);
}

TEST(IvfRabitqBenchCommonTest, LoadVecsRejectsInconsistentDimension) {
  std::filesystem::path path = TempVecsPath();
  {
    std::ofstream output(path, std::ios::binary);
    int32_t first_dim = 2;
    float first_values[] = {1.0F, 2.0F};
    int32_t second_dim = 1;
    float second_values[] = {3.0F, 4.0F};
    output.write(reinterpret_cast<const char *>(&first_dim), sizeof(first_dim));
    output.write(reinterpret_cast<const char *>(first_values),
                 sizeof(first_values));
    output.write(reinterpret_cast<const char *>(&second_dim),
                 sizeof(second_dim));
    output.write(reinterpret_cast<const char *>(second_values),
                 sizeof(second_values));
  }

  Vecs<float> result;
  std::string error;
  EXPECT_FALSE(LoadVecs(path.string(), 0, &result, &error));
  EXPECT_NE(error.find("inconsistent vecs dimension"), std::string::npos);
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
}

TEST(IvfRabitqBenchCommonTest, LoadCohereTextVecs) {
  std::filesystem::path path = TempPath(".txt");
  {
    std::ofstream output(path);
    output << "0;1.5 2.5 3.5;\n";
    output << "1;4.5 5.5 6.5;\n";
  }

  Vecs<float> result;
  std::string error;
  ASSERT_TRUE(LoadCohereTextVecs(path.string(), 0, &result, &error)) << error;
  EXPECT_EQ(2U, result.count);
  EXPECT_EQ(3U, result.dim);
  EXPECT_EQ((std::vector<float>{1.5F, 2.5F, 3.5F, 4.5F, 5.5F, 6.5F}),
            result.values);
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
}

TEST(IvfRabitqBenchCommonTest, LoadZvecFloatVecsPreservesKeys) {
  std::filesystem::path path = TempPath(".zvec.vecs");
  {
    std::ofstream output(path, std::ios::binary);
    ZvecVecsHeader header{};
    header.num_vecs = 2;
    header.version = 1;
    header.meta_size = sizeof(ZvecIndexMetaHeader);
    header.key_offset = 4 * sizeof(float);
    header.key_size = 2 * sizeof(uint64_t);
    ZvecIndexMetaHeader meta{};
    meta.header_size = sizeof(meta);
    meta.data_type = 2;
    meta.dimension = 2;
    meta.unit_size = sizeof(float);
    float values[] = {1.0F, 2.0F, 3.0F, 4.0F};
    uint64_t keys[] = {42, 7};
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(reinterpret_cast<const char *>(&meta), sizeof(meta));
    output.write(reinterpret_cast<const char *>(values), sizeof(values));
    output.write(reinterpret_cast<const char *>(keys), sizeof(keys));
  }

  Vecs<float> result;
  std::string error;
  ASSERT_TRUE(LoadZvecFloatVecs(path.string(), 0, &result, &error)) << error;
  EXPECT_EQ((std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F}), result.values);
  EXPECT_EQ((std::vector<uint64_t>{42, 7}), result.keys);
  EXPECT_EQ(42U, KeyAt(result, 0));
  EXPECT_EQ(7U, KeyAt(result, 1));
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
}

TEST(IvfRabitqBenchCommonTest, NormalizeRowsForCosine) {
  Vecs<float> vectors;
  vectors.count = 2;
  vectors.dim = 2;
  vectors.values = {3.0F, 4.0F, 0.0F, 5.0F};

  std::string error;
  ASSERT_TRUE(NormalizeRows(&vectors, &error)) << error;
  EXPECT_FLOAT_EQ(0.6F, vectors.values[0]);
  EXPECT_FLOAT_EQ(0.8F, vectors.values[1]);
  EXPECT_FLOAT_EQ(0.0F, vectors.values[2]);
  EXPECT_FLOAT_EQ(1.0F, vectors.values[3]);
}

TEST(IvfRabitqBenchCommonTest, RecallAtKUsesTopKIntersection) {
  Vecs<int32_t> groundtruth;
  groundtruth.count = 2;
  groundtruth.dim = 3;
  groundtruth.values = {1, 2, 3, 4, 5, 6};
  std::vector<uint64_t> actual{2, 8, 4, 5};
  EXPECT_DOUBLE_EQ(RecallAtK(actual, groundtruth, 2, 2), 0.75);
}

}  // namespace
}  // namespace ivf_rabitq_bench
