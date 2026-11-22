// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <zvec/db/collection.h>

namespace zvec::internal {
// Explicit development entry for gated run-format collections. Opening an
// existing collection always uses its committed format.
Result<Collection::Ptr> CreateFtsWritingCollection(
    const std::string &path, const CollectionSchema &schema,
    const CollectionOptions &options);
Status MigrateFtsWritingCollection(Collection &collection);
// The observation runs after each applied record, before recovery can publish
// or return a handle. Crash tests use it to terminate the recovering process.
Result<Collection::Ptr> OpenWithWalReplayObserver(
    const std::string &path, const CollectionOptions &options,
    const std::function<void(uint64_t)> &on_replayed);
Status SnapshotFtsWritingCollection(Collection &collection,
                                    const std::string &destination);
}  // namespace zvec::internal
