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
