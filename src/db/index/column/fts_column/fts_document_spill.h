// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <zvec/db/schema.h>
#include "fts_file_lifetime.h"
#include "fts_source.h"

namespace zvec::fts {

// The returned sources share a private one-row spool. Consume the fields
// serially during candidate encoding, before appending the database WAL.
Result<std::vector<std::vector<FtsFieldSource::Ptr>>> PrepareSpilledFtsDocument(
    const FieldSchemaPtrList &fields,
    const std::vector<std::optional<std::string_view>> &text,
    const std::string &segment_path, uint32_t block,
    const FtsRetirementQueue::Pointer &retirement, size_t prepare_bytes,
    uint64_t disk_bytes);

}  // namespace zvec::fts
