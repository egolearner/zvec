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

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <zvec/ailego/buffer/block_eviction_queue.h>
#include <zvec/ailego/io/file.h>
#include <zvec/ailego/utility/file_helper.h>
#include "db/common/file_helper.h"
#include "db/fts_writing_collection.h"
#include "db/index/common/version_manager.h"
#include "zvec/db/collection.h"
#include "zvec/db/doc.h"
#include "zvec/db/index_params.h"
#include "zvec/db/options.h"
#include "zvec/db/schema.h"
#include "zvec/db/status.h"
#include "zvec/db/type.h"

using namespace zvec;

static const std::string kTestPath = "./test_fts_query";

class FtsQueryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ailego::MemoryLimitPool::get_instance().init(64u << 20));
    FileHelper::RemoveDirectory(kTestPath);
  }
  void TearDown() override {
    FileHelper::RemoveDirectory(kTestPath);
  }

  // Create a schema with one STRING field (for forward) and one FTS field.
  static CollectionSchema::Ptr CreateFtsSchema() {
    auto schema = std::make_shared<CollectionSchema>("fts_demo");
    // A simple scalar field for forward store
    schema->add_field(std::make_shared<FieldSchema>("title", DataType::STRING));
    // FTS indexed field
    schema->add_field(
        std::make_shared<FieldSchema>("content", DataType::STRING, false,
                                      std::make_shared<FtsIndexParams>()));
    // A vector field is required for Collection to work (segment open expects
    // at least one vector field in the normal schema path).
    schema->add_field(std::make_shared<FieldSchema>(
        "vec", DataType::VECTOR_FP32, 4, false,
        std::make_shared<FlatIndexParams>(MetricType::IP)));
    return schema;
  }

  static CollectionSchema::Ptr CreateNGramFtsSchema() {
    auto schema = std::make_shared<CollectionSchema>("ngram_fts_demo");
    schema->add_field(std::make_shared<FieldSchema>("title", DataType::STRING));
    auto fts_params = std::make_shared<FtsIndexParams>(
        "ngram", std::vector<std::string>{},
        R"({"ngram_min":2,"ngram_max":2,"token_chars":["letter"]})");
    schema->add_field(std::make_shared<FieldSchema>(
        "content", DataType::STRING, false, std::move(fts_params)));
    schema->add_field(std::make_shared<FieldSchema>(
        "vec", DataType::VECTOR_FP32, 4, false,
        std::make_shared<FlatIndexParams>(MetricType::IP)));
    return schema;
  }

  static Doc MakeDoc(uint64_t id, const std::string &title,
                     const std::string &content) {
    Doc doc;
    doc.set_pk("pk_" + std::to_string(id));
    doc.set<std::string>("title", title);
    doc.set<std::string>("content", content);
    // dummy vector
    doc.set<std::vector<float>>("vec", std::vector<float>(4, float(id + 0.1)));
    return doc;
  }
};

TEST_F(FtsQueryTest, BasicFtsQuery) {
  auto schema = CreateFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto col = result.value();

  // Insert documents
  std::vector<Doc> docs;
  docs.push_back(MakeDoc(0, "intro", "hello world from zvec"));
  docs.push_back(MakeDoc(1, "guide", "hello foo bar"));
  docs.push_back(MakeDoc(2, "faq", "baz qux nothing here"));
  docs.push_back(MakeDoc(3, "tips", "hello hello hello world"));

  auto insert_res = col->insert(docs);
  ASSERT_TRUE(insert_res.has_value()) << insert_res.error().message();

  // FTS query: search for "hello"
  SearchQuery vq;
  vq.target_.field_name_ = "content";
  vq.topk_ = 10;
  FtsClause fts;
  fts.query_string_ = "hello";
  vq.target_.clause_ = fts;

  auto query_res = col->query(vq);
  ASSERT_TRUE(query_res.has_value()) << query_res.error().message();

  auto &results = query_res.value();
  // Documents 0, 1, 3 contain "hello"; document 2 does not.
  ASSERT_GE(results.size(), 2u);
  ASSERT_LE(results.size(), 3u);
}

TEST_F(FtsQueryTest, RunWriterCheckpointReplacementAndSeal) {
  for (bool mmap : {false, true}) {
    SCOPED_TRACE(mmap);
    auto schema = CreateFtsSchema();
    schema->set_max_doc_count_per_segment(1000);
    CollectionOptions options{false, mmap};
    auto created =
        internal::CreateFtsWritingCollection(kTestPath, *schema, options);
    ASSERT_TRUE(created) << created.error().message();
    auto collection = std::move(*created);
    auto check = [&](const std::string &term,
                     const std::set<std::string> &expected) {
      SearchQuery query;
      query.target_.field_name_ = "content";
      FtsClause clause;
      clause.query_string_ = term;
      query.target_.clause_ = clause;
      query.topk_ = 10;
      auto result = collection->query(query);
      ASSERT_TRUE(result) << result.error().message();
      std::set<std::string> actual;
      for (const auto &doc : *result) {
        actual.insert(doc->pk_ref());
      }
      EXPECT_EQ(actual, expected);
    };
    std::vector<Doc> docs{MakeDoc(1, "one", "red red"),
                          MakeDoc(2, "two", "red blue")};
    auto written = collection->insert(docs);
    ASSERT_TRUE(written) << written.error().message();
    ASSERT_EQ(written->size(), 2);
    ASSERT_TRUE(written->at(0).ok());
    ASSERT_TRUE(written->at(1).ok());
    check("red", {"pk_1", "pk_2"});
    auto status = collection->flush();
    ASSERT_TRUE(status.ok()) << status.message();
    check("red", {"pk_1", "pk_2"});
    docs = {MakeDoc(1, "replaced", "green")};
    auto changed = collection->upsert(docs);
    ASSERT_TRUE(changed && changed->front().ok());
    check("red", {"pk_2"});
    check("green", {"pk_1"});
    status = collection->close();
    ASSERT_TRUE(status.ok()) << status.message();
    collection.reset();
    auto reopened = Collection::Open(kTestPath, options);
    ASSERT_TRUE(reopened) << reopened.error().message();
    collection = std::move(*reopened);
    check("green", {"pk_1"});
    auto deleted = collection->delete_({"pk_2"});
    ASSERT_TRUE(deleted && deleted->front().ok());
    status = collection->flush();  // delete-only epoch
    ASSERT_TRUE(status.ok()) << status.message();
    check("red", {});
    docs = {MakeDoc(3, "new", "green")};
    for (uint64_t id = 5; id < 1001; ++id) {
      docs.push_back(MakeDoc(id, "padding", "neutral"));
    }
    docs.push_back(MakeDoc(4, "successor", "green"));
    written = collection->insert(docs);
    ASSERT_TRUE(written) << written.error().message();
    for (const auto &entry : *written) {
      ASSERT_TRUE(entry.ok()) << entry.message();
    }
    check("green", {"pk_1", "pk_3", "pk_4"});
    status = collection->close();
    ASSERT_TRUE(status.ok()) << status.message();
    collection.reset();
    auto version = VersionManager::Recovery(kTestPath);
    ASSERT_TRUE(version) << version.error().message();
    auto current = (*version)->get_current_version();
    EXPECT_TRUE(current.use_fts_runs());
    ASSERT_TRUE(current.writing_segment_meta()->writing_fts_checkpoint());
    EXPECT_EQ(
        current.writing_segment_meta()->writing_fts_checkpoint()->row_frontier,
        1);
    ASSERT_EQ(current.persisted_segment_metas().size(), 1);
    bool sealed = false;
    for (const auto &block :
         current.persisted_segment_metas().front()->persisted_blocks()) {
      if (block.type() == BlockType::FTS_INDEX) {
        sealed = block.fts_base_.has_value();
      }
    }
    EXPECT_TRUE(sealed);
    FileHelper::RemoveDirectory(kTestPath);
  }
}

TEST_F(FtsQueryTest, LegacyMigrationAndIndependentSnapshot) {
  for (bool mmap : {true, false}) {
    SCOPED_TRACE(mmap);
    auto schema = CreateFtsSchema();
    CollectionOptions options{false, mmap};
    auto created = Collection::CreateAndOpen(kTestPath, *schema, options);
    ASSERT_TRUE(created) << created.error().message();
    auto source = std::move(*created);
    std::vector<Doc> docs{MakeDoc(0, "one", "green green blue"),
                          MakeDoc(1, "two", "green blue")};
    auto inserted = source->insert(docs);
    ASSERT_TRUE(inserted && inserted->size() == 2);
    for (const auto &row : *inserted) {
      ASSERT_TRUE(row.ok()) << row.message();
    }
    SearchQuery query;
    query.target_.field_name_ = "content";
    FtsClause clause;
    clause.query_string_ = "green";
    query.target_.clause_ = clause;
    query.topk_ = 10;
    auto before = source->query(query);
    ASSERT_TRUE(before && before->size() == 2);
    auto status = internal::MigrateFtsWritingCollection(*source);
    ASSERT_TRUE(status.ok()) << status.message();
    auto compare = [&](Collection &collection) {
      auto after = collection.query(query);
      ASSERT_TRUE(after) << after.error().message();
      ASSERT_EQ(after->size(), before->size());
      for (size_t i = 0; i < before->size(); ++i) {
        EXPECT_EQ(after->at(i)->pk_ref(), before->at(i)->pk_ref());
        EXPECT_EQ(after->at(i)->score(), before->at(i)->score());
      }
    };
    compare(*source);
    auto snapshot = kTestPath + ".snapshot";
    FileHelper::RemoveDirectory(snapshot);
    status = internal::SnapshotFtsWritingCollection(*source, snapshot);
    ASSERT_TRUE(status.ok()) << status.message();
    auto deleted = source->delete_({"pk_0"});
    ASSERT_TRUE(deleted && deleted->front().ok());
    status = source->close();
    ASSERT_TRUE(status.ok()) << status.message();
    source.reset();
    FileHelper::RemoveDirectory(kTestPath);
    auto copy = Collection::Open(snapshot, CollectionOptions{true, !mmap});
    ASSERT_TRUE(copy) << copy.error().message();
    compare(**copy);
    status = (*copy)->close();
    ASSERT_TRUE(status.ok()) << status.message();
    FileHelper::RemoveDirectory(snapshot);
  }
}

TEST_F(FtsQueryTest, EmptyWritingSnapshotCanAcceptTheFirstRow) {
  auto schema = CreateFtsSchema();
  CollectionOptions options{false, true};
  auto source =
      internal::CreateFtsWritingCollection(kTestPath, *schema, options);
  ASSERT_TRUE(source) << source.error().message();
  auto snapshot = kTestPath + ".snapshot";
  FileHelper::RemoveDirectory(snapshot);
  auto status = internal::SnapshotFtsWritingCollection(**source, snapshot);
  ASSERT_TRUE(status.ok()) << status.message();
  auto copy = Collection::Open(snapshot, options);
  ASSERT_TRUE(copy) << copy.error().message();
  std::vector<Doc> docs{MakeDoc(1, "first", "green")};
  auto inserted = (*copy)->insert(docs);
  ASSERT_TRUE(inserted && inserted->front().ok());
  status = (*copy)->close();
  ASSERT_TRUE(status.ok()) << status.message();
  FileHelper::RemoveDirectory(snapshot);
}

TEST_F(FtsQueryTest, RunFormatRebuildsAndDropsIndexesAcrossRollover) {
  auto schema = CreateFtsSchema();
  CollectionOptions options{false, true};
  auto col = internal::CreateFtsWritingCollection(kTestPath, *schema, options);
  ASSERT_TRUE(col) << col.error().message();
  std::vector<Doc> batch{MakeDoc(0, "quick fox", "red green")};
  ASSERT_TRUE((*col)->insert(batch)->front().ok());
  auto status =
      (*col)->create_index("title", std::make_shared<FtsIndexParams>());
  ASSERT_TRUE(status.ok()) << status.message();
  batch = {MakeDoc(1, "quick bird", "blue")};
  ASSERT_TRUE((*col)->insert(batch)->front().ok());
  auto query = [](const std::string &field, const std::string &text) {
    SearchQuery request;
    request.target_.field_name_ = field;
    FtsClause clause;
    clause.query_string_ = text;
    request.target_.clause_ = clause;
    request.topk_ = 10;
    return request;
  };
  auto both = (*col)->query(query("title", "quick"));
  ASSERT_TRUE(both) << both.error().message();
  ASSERT_EQ(both->size(), 2);
  status = (*col)->create_index("title",
                                std::make_shared<FtsIndexParams>("whitespace"));
  ASSERT_TRUE(status.ok()) << status.message();
  status = (*col)->drop_index("content");
  ASSERT_TRUE(status.ok()) << status.message();
  status = (*col)->close();
  ASSERT_TRUE(status.ok()) << status.message();
  col = Collection::Open(kTestPath, options);
  ASSERT_TRUE(col) << col.error().message();
  both = (*col)->query(query("title", "quick"));
  ASSERT_TRUE(both) << both.error().message();
  EXPECT_EQ(both->size(), 2);
  EXPECT_FALSE((*col)->query(query("content", "green")).has_value());
  status = (*col)->drop_index("title");
  ASSERT_TRUE(status.ok()) << status.message();
  status = (*col)->create_index("content", std::make_shared<FtsIndexParams>());
  ASSERT_TRUE(status.ok()) << status.message();
  auto one = (*col)->query(query("content", "green"));
  ASSERT_TRUE(one) << one.error().message();
  ASSERT_EQ(one->size(), 1);
  EXPECT_EQ(one->front()->pk_ref(), "pk_0");
}

TEST_F(FtsQueryTest, ConcurrentReadsNeverObserveAPartialReplacement) {
  auto schema = CreateFtsSchema();
  auto col =
      internal::CreateFtsWritingCollection(kTestPath, *schema, {false, true});
  ASSERT_TRUE(col) << col.error().message();
  std::vector<Doc> docs{MakeDoc(1, "one", "shared first")};
  ASSERT_TRUE((*col)->insert(docs)->front().ok());
  SearchQuery query;
  query.target_.field_name_ = "content";
  FtsClause clause;
  clause.query_string_ = "shared";
  query.target_.clause_ = clause;
  query.topk_ = 10;
  std::atomic<bool> started{false};
  std::thread reader([&] {
    started = true;
    for (size_t i = 0; i < 100; ++i) {
      auto result = (*col)->query(query);
      ASSERT_TRUE(result) << result.error().message();
      ASSERT_EQ(1u, result->size());
      EXPECT_EQ("pk_1", result->front()->pk_ref());
    }
  });
  while (!started.load()) {
    std::this_thread::yield();
  }
  for (size_t i = 0; i < 30; ++i) {
    docs = {MakeDoc(1, "one", i % 2 ? "shared second" : "shared first")};
    auto written = (*col)->upsert(docs);
    EXPECT_TRUE(written && written->front().ok());
    if (i % 5 == 0) {
      EXPECT_TRUE((*col)->flush().ok());
    }
  }
  reader.join();
}

TEST_F(FtsQueryTest, NGramTokenizerEndToEnd) {
  auto schema = CreateNGramFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto col = result.value();

  std::vector<Doc> docs;
  docs.push_back(MakeDoc(0, "chinese",
                         "\xE4\xB8\xAD\xE6\x96\x87\xE5\x88\x86\xE8\xAF\x8D"));
  docs.push_back(MakeDoc(1, "english", "english tokenizer"));
  auto insert_result = col->insert(docs);
  ASSERT_TRUE(insert_result.has_value()) << insert_result.error().message();

  SearchQuery query;
  query.target_.field_name_ = "content";
  query.topk_ = 10;
  FtsClause fts;
  fts.query_string_ = "\xE4\xB8\xAD\xE6\x96\x87";
  query.target_.clause_ = fts;

  auto query_result = col->query(query);
  ASSERT_TRUE(query_result.has_value()) << query_result.error().message();
  ASSERT_EQ(query_result.value().size(), 1u);
}

TEST_F(FtsQueryTest, NGramConfigErrorIsReturnedByCreateCollection) {
  auto schema = CreateNGramFtsSchema();
  auto field = schema->get_field("content");
  auto params =
      std::dynamic_pointer_cast<FtsIndexParams>(field->index_params());
  ASSERT_NE(params, nullptr);
  params->set_extra_params(R"({"ngram_min":1,"ngram_max":3})");

  CollectionOptions options;
  options.read_only_ = false;
  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);

  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("ngram_max - ngram_min must be <= 1"),
            std::string::npos);
}

TEST_F(FtsQueryTest, FtsQueryEmptyField) {
  auto schema = CreateFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value());
  auto col = result.value();

  SearchQuery vq;
  vq.target_.field_name_ = "";  // empty
  vq.topk_ = 10;
  FtsClause fts;
  fts.query_string_ = "hello";
  vq.target_.clause_ = fts;

  auto query_res = col->query(vq);
  ASSERT_FALSE(query_res.has_value());
}

TEST_F(FtsQueryTest, FtsQueryNoMatch) {
  auto schema = CreateFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value());
  auto col = result.value();

  std::vector<Doc> docs;
  docs.push_back(MakeDoc(0, "intro", "hello world"));
  auto insert_res = col->insert(docs);
  ASSERT_TRUE(insert_res.has_value());

  SearchQuery vq;
  vq.target_.field_name_ = "content";
  vq.topk_ = 10;
  FtsClause fts;
  fts.query_string_ = "nonexistent_term_xyz";
  vq.target_.clause_ = fts;

  auto query_res = col->query(vq);
  ASSERT_TRUE(query_res.has_value());
  ASSERT_EQ(query_res.value().size(), 0u);
}

// Verify that FTS fields do NOT support add/alter/drop column operations.
// The schema change validation only allows basic numeric types [INT32..DOUBLE].
TEST_F(FtsQueryTest, FtsFieldUnsupportedAddColumn) {
  auto schema = CreateFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value());
  auto col = result.value();

  // Insert a document so the collection is non-empty
  std::vector<Doc> docs;
  docs.push_back(MakeDoc(0, "intro", "hello world"));
  auto insert_res = col->insert(docs);
  ASSERT_TRUE(insert_res.has_value());
  ASSERT_TRUE(col->flush().ok());

  // Attempt to add a new FTS column — should fail
  auto fts_field = std::make_shared<FieldSchema>(
      "new_fts", DataType::STRING, true, std::make_shared<FtsIndexParams>());
  auto status = col->add_column(fts_field, "", AddColumnOptions());
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.code(), StatusCode::INVALID_ARGUMENT);
}

TEST_F(FtsQueryTest, FtsFieldUnsupportedDropColumn) {
  auto schema = CreateFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value());
  auto col = result.value();

  // Insert a document so the collection is non-empty
  std::vector<Doc> docs;
  docs.push_back(MakeDoc(0, "intro", "hello world"));
  auto insert_res = col->insert(docs);
  ASSERT_TRUE(insert_res.has_value());
  ASSERT_TRUE(col->flush().ok());

  // Attempt to drop an existing FTS column — should fail
  auto status = col->drop_column("content");
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.code(), StatusCode::INVALID_ARGUMENT);
}

TEST_F(FtsQueryTest, FtsFieldUnsupportedAlterColumn) {
  auto schema = CreateFtsSchema();
  CollectionOptions options;
  options.read_only_ = false;

  auto result = Collection::CreateAndOpen(kTestPath, *schema, options);
  ASSERT_TRUE(result.has_value());
  auto col = result.value();

  // Insert a document so the collection is non-empty
  std::vector<Doc> docs;
  docs.push_back(MakeDoc(0, "intro", "hello world"));
  auto insert_res = col->insert(docs);
  ASSERT_TRUE(insert_res.has_value());
  ASSERT_TRUE(col->flush().ok());

  // Attempt to alter (rename) the FTS column — should fail
  auto status = col->alter_column("content", "content_renamed", nullptr,
                                  AlterColumnOptions());
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.code(), StatusCode::INVALID_ARGUMENT);

  // Attempt to alter the FTS column with a new schema — should also fail
  auto new_fts_field = std::make_shared<FieldSchema>(
      "content", DataType::STRING, true, std::make_shared<FtsIndexParams>());
  status =
      col->alter_column("content", "", new_fts_field, AlterColumnOptions());
  ASSERT_FALSE(status.ok());
  ASSERT_EQ(status.code(), StatusCode::INVALID_ARGUMENT);
}
