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

#include "fts_reader.h"

namespace zvec::fts {

constexpr size_t kMaxWritingRuns = 16;

// The inputs cover consecutive, disjoint local domains starting at parent
// row zero. A caller must hold the collection read barrier for mutable inputs.
Result<FtsSegmentReader::Ptr> OpenWritingFtsReader(
    std::vector<FtsSegmentReader::Ptr> inputs, std::vector<uint32_t> rows);

}  // namespace zvec::fts
