// Copyright 2025-present the zvec project
// SPDX-License-Identifier: Apache-2.0

#include "db/index/column/fts_column/fts_active_buffer.h"
#include <libstemmer.h>
#include <gtest/gtest.h>
#include "db/common/file_helper.h"
#include "db/index/column/fts_column/fts_document_spill.h"
#include "db/index/column/fts_column/fts_writing_index.h"
#include "db/index/column/fts_column/fts_writing_reader.h"
#include "db/index/column/fts_column/tokenizer/stemmer_token_filter.h"
#include "db/index/column/fts_column/tokenizer/tokenizer_factory.h"

namespace zvec::fts {
namespace {

void ExpectAnalysis(const FtsIndexParams &params, const std::string &input,
                    size_t budget) {
  auto pipeline = TokenizerFactory::create(params);
  ASSERT_TRUE(pipeline);
  const auto expected = (*pipeline)->process(input);
  std::vector<Token> actual;
  auto status = (*pipeline)->analyze_to(
      input,
      [&](std::string_view word, uint32_t off, uint32_t pos) {
        actual.push_back({std::string(word), off, pos});
        return Status::OK();
      },
      budget);
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual[i].text, expected[i].text);
    EXPECT_EQ(actual[i].offset, expected[i].offset);
    EXPECT_EQ(actual[i].position, expected[i].position);
  }
}

TEST(ActiveBufferTest, ChunkedAnalyzerMatchesExistingTokenSequence) {
  std::string input;
  for (size_t i = 0; i < 400; ++i) {
    input.append("Hello __A.B__ 22,111 世界 🌎 Straße café.\t");
  }
  input.append(1, static_cast<char>(0xfe));
  for (const auto *name : {"standard", "whitespace", "ngram"}) {
    SCOPED_TRACE(name);
    FtsIndexParams params{name, {"ascii_folding", "lowercase"}, ""};
    if (params.tokenizer_name == "ngram") {
      params.extra_params =
          R"({"ngram_min":2,"ngram_max":3,"token_chars":["letter","digit"]})";
    }
    ExpectAnalysis(params, input, 128 * 1024);
  }
}

TEST(ActiveBufferTest, LongUnicodeTextMatchesTermPositionsAndByteOffsets) {
  std::string input;
  for (size_t i = 0; i < 22000; ++i) {
    input += "中";
  }
  FtsIndexParams params{"standard", {"lowercase"}, ""};
  ExpectAnalysis(params, input, 128 * 1024);
  input.clear();
  for (size_t i = 0; i < 1000; ++i) {
    input += "中\u0301あA.B12,456👨‍👩‍👧‍👦";
  }
  ExpectAnalysis(params, input, 128 * 1024);
  input.clear();
  for (size_t i = 0; i < 1500; ++i) {
    input += "α,β。👨‍👩‍👧‍👦🇨🇳3️⃣";
  }
  ExpectAnalysis(params, input, 128 * 1024);
}

TEST(ActiveBufferTest, LongCjkDocumentCanBeAppliedInMemoryAndThroughSpill) {
  const std::string path{"fts_cjk_spill.fixture"};
  ASSERT_TRUE(FileHelper::CreateDirectory(path));
  auto retirement = FtsRetirementQueue::Create();
  auto cleanup = retirement->track(path, true, true);
  CollectionSchema schema("cjk");
  auto field = std::make_shared<FieldSchema>(
      "body", DataType::STRING, true,
      std::make_shared<zvec::FtsIndexParams>("standard"));
  ASSERT_TRUE(schema.add_field(field).ok());
  std::string input;
  for (size_t i = 0; i < 22000; ++i) {
    input += "中";
  }
  for (const bool spill : {false, true}) {
    FtsWritingOptions options;
    options.active_bytes = spill ? 16384 : 4u << 20;
    options.prepare_bytes = 4u << 20;
    auto budget = std::make_shared<FtsReadBudget>(8u << 20);
    auto writer = FtsWritingIndex::Open(path, 1, 2, true, schema, retirement,
                                        budget, options);
    ASSERT_TRUE(writer);
    auto doc = (*writer)->prepare({input});
    ASSERT_TRUE(doc) << doc.error().message();
    (*writer)->apply(**doc);
    EXPECT_EQ(spill, (*writer)->run_file_bytes() != 0);
    auto root = (*writer)->checkpoint(1);
    ASSERT_TRUE(root && root->runs.size() == 1);
    auto run =
        FtsRun::Open(path, true, 1, 2, root->runs.front(), retirement, budget);
    ASSERT_TRUE(run);
    auto reader = (*run)->reader(0);
    ASSERT_TRUE(reader);
    auto term = (*reader)->lookup("中");
    ASSERT_TRUE(term && *term);
    auto postings = (*term)->postings();
    ASSERT_TRUE(postings);
    ASSERT_EQ(0u, *(*postings)->next());
    EXPECT_EQ(22000u, (*postings)->payload()->tf);
    auto positions = (*term)->positions(0);
    ASSERT_TRUE(positions);
    EXPECT_EQ(21999u, **(*positions)->seek_ge(21999));
  }
}

TEST(ActiveBufferTest, JiebaNaturalUnitsAndMalformedTail) {
  std::string input = "我爱中华人民共和国，南京市长江大桥。Java SDK\tword ";
  input.append(1, '\0');
  input.append(" 计算机语言\n\rCoding!");
  for (const auto *mode : {"mix", "search", "full", "hmm"}) {
    SCOPED_TRACE(mode);
    FtsIndexParams params{
        "jieba", {"lowercase"}, std::string("{\"cut_mode\":\"") + mode + "\"}"};
    ExpectAnalysis(params, input, 128 * 1024);
    ExpectAnalysis(params, input + static_cast<char>(0xff), 128 * 1024);
  }
}

TEST(ActiveBufferTest, LowercaseStreamsUnchangedMultiByteAndLongTokens) {
  FtsIndexParams params{"whitespace", {"lowercase"}, ""};
  std::string input = "A_WORLD ПрИвЕт İKΣ straße ẞ ";
  input.push_back(static_cast<char>(0xff));
  input += "A";
  ExpectAnalysis(params, input, 128 * 1024);
  ExpectAnalysis(params, std::string(100000, 'a'), 128 * 1024);
}

TEST(ActiveBufferTest, BoundedSnowballMatchesAllInstalledLanguages) {
  for (auto language = sb_stemmer_list(); *language; ++language) {
    SCOPED_TRACE(*language);
    FtsIndexParams params{
        "whitespace",
        {"lowercase", "stemmer"},
        std::string("{\"stemmer_lang\":\"") + *language + "\"}"};
    ExpectAnalysis(params,
                   "Runners Bücher größer singing käyttäminen запустить "
                   "کتاب‌ها",
                   128 * 1024);
  }
  StemmerTokenFilter filter;
  ASSERT_TRUE(filter.init({}));
  std::vector<Token> one{{std::string(8192, 'a'), 0, 0}};
  EXPECT_EQ(filter.filter_one(&one, 4097).code(),
            StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_TRUE(filter.filter_one(&one, 65536).ok());
}

TEST(ActiveBufferTest,
     ModelInputAdmissionSharesConfigurationAndReleasesLiveTries) {
  FtsIndexParams params{"jieba", {"lowercase"}, ""};
  auto first = TokenizerFactory::reserve_model(params);
  ASSERT_TRUE(first && *first);
  auto same = TokenizerFactory::reserve_model(params);
  ASSERT_TRUE(same);
  EXPECT_EQ(*first, *same);
  std::vector<std::shared_ptr<void>> leases;
  for (unsigned i = 0; i < 1024; ++i) {
    params.extra_params = R"({"distinct":)" + std::to_string(i) + "}";
    auto lease = TokenizerFactory::reserve_model(params);
    if (!lease) {
      EXPECT_EQ(lease.error().code(), StatusCode::RESOURCE_EXHAUSTED);
      break;
    }
    leases.push_back(std::move(*lease));
  }
  ASSERT_FALSE(leases.empty());
  EXPECT_LT(leases.size(), 1024u);
  leases.clear();
  EXPECT_TRUE(TokenizerFactory::reserve_model(params));
}

std::shared_ptr<FtsActiveBuffer> MakeBuffer(
    size_t active_bytes = 64 * 1024,
    std::shared_ptr<FtsWriteBudget> budget = {}) {
  auto params = std::make_shared<zvec::FtsIndexParams>(
      "whitespace", std::vector<std::string>{});
  FieldSchemaPtrList fields;
  for (const auto *name : {"first", "second"}) {
    fields.push_back(
        std::make_shared<FieldSchema>(name, DataType::STRING, true, params));
  }
  auto buffer = FtsActiveBuffer::Create(fields, active_bytes, 1024 * 1024,
                                        std::move(budget));
  return buffer ? *buffer : nullptr;
}

TEST(ActiveBufferTest, ConcurrentPostingBatchesShareTheReadLimit) {
  auto budget = std::make_shared<FtsReadBudget>(3000);
  auto params = std::make_shared<zvec::FtsIndexParams>("standard");
  auto active = FtsActiveBuffer::Create(
      {std::make_shared<FieldSchema>("body", DataType::STRING, true, params)},
      65536, 1u << 20, {}, budget);
  ASSERT_TRUE(active);
  auto doc = (*active)->prepare({"one two"});
  ASSERT_TRUE(doc);
  (*active)->apply(**doc);
  auto part = (*active)->reader(0);
  ASSERT_TRUE(part);
  auto reader = OpenWritingFtsReader({*part}, {1}, budget);
  ASSERT_TRUE(reader);
  auto first = (*reader)->lookup("one");
  auto second = (*reader)->lookup("two");
  ASSERT_TRUE(first && second);
  {
    auto a = (*first)->postings();
    ASSERT_TRUE(a && (*a)->next_batch());
    auto b = (*second)->postings();
    ASSERT_TRUE(b);
    auto denied = (*b)->next_batch();
    ASSERT_FALSE(denied);
    EXPECT_EQ(StatusCode::RESOURCE_EXHAUSTED, denied.error().code());
  }
  auto retry = (*second)->postings();
  ASSERT_TRUE(retry && (*retry)->next_batch());
}

TEST(ActiveBufferTest, CollectionAdmissionFollowsPrepareAndPinnedFrozenOwner) {
  EXPECT_FALSE(
      MakeBuffer(65536, std::make_shared<FtsWriteBudget>(16384, 4096)));
  auto budget = std::make_shared<FtsWriteBudget>(65536, 1024 * 1024);
  auto first = MakeBuffer(65536, budget);
  auto second = MakeBuffer(65536, budget);
  ASSERT_TRUE(first && second);
  auto doc = first->prepare({"first", std::nullopt});
  ASSERT_TRUE(doc);
  EXPECT_FALSE(second->prepare({"blocked by prepare", std::nullopt}));
  first->apply(**doc);
  EXPECT_FALSE(second->prepare({"blocked by active", std::nullopt}));
  auto source = first->source(0);
  ASSERT_TRUE(source);
  first.reset();
  auto other = second->prepare({"second", std::nullopt});
  ASSERT_TRUE(other);
  second->apply(**other);
  EXPECT_FALSE(second->source(0));
  auto after = second->prepare({"still mutable", std::nullopt});
  ASSERT_TRUE(after);
  second->apply(**after);
  source->reset();
  ASSERT_TRUE(second->source(0));
  second.reset();
  const auto usage = budget->usage();
  for (size_t i = 0; i < usage.current.size(); ++i) {
    EXPECT_EQ(usage.current[i], 0u);
    if (i < static_cast<size_t>(FtsWriteBudget::Part::Flush)) {
      EXPECT_GT(usage.peak[i], 0u);
    }
  }
  EXPECT_LE(usage.peak[static_cast<size_t>(FtsWriteBudget::Part::Frozen)],
            65536u);
  EXPECT_GT(usage.peak_prepare_vectors, 0u);
}

TEST(ActiveBufferTest, CancellationAndAtomicMultifieldApply) {
  auto buffer = MakeBuffer();
  ASSERT_TRUE(buffer);
  {
    auto prepared = buffer->prepare({"never visible", "rollback"});
    ASSERT_TRUE(prepared);
    auto view = buffer->reader(0);
    ASSERT_TRUE(view);
    auto hidden = (*view)->lookup("never");
    ASSERT_TRUE(hidden);
    EXPECT_FALSE(*hidden);
    EXPECT_FALSE(buffer->prepare({"concurrent", std::nullopt}));
    EXPECT_FALSE(buffer->source(0));
  }
  EXPECT_EQ(buffer->rows(), 0u);
  const std::string huge(65536, 'x');
  EXPECT_FALSE(buffer->prepare({"must roll back", huge}));
  EXPECT_EQ(buffer->rows(), 0u);
  auto first = buffer->prepare({"a a a a a a a a a a a", std::nullopt});
  ASSERT_TRUE(first);
  buffer->apply(**first);
  auto second = buffer->prepare({std::nullopt, ""});
  ASSERT_TRUE(second);
  buffer->apply(**second);
  auto third = buffer->prepare({"b a", "c"});
  ASSERT_TRUE(third);
  buffer->apply(**third);
  auto source = buffer->source(0);
  auto empty = buffer->source(1);
  ASSERT_TRUE(source && empty);
  EXPECT_FALSE(buffer->prepare({"frozen", "cannot change"}));
  EXPECT_EQ((*source)->statistics().scoring_documents, 2u);
  EXPECT_EQ((*source)->statistics().total_tokens, 13u);
  ASSERT_TRUE((*empty)->document(0));
  EXPECT_FALSE((*empty)->document(0)->present);
  EXPECT_TRUE((*empty)->document(1)->present);
  EXPECT_EQ((*empty)->document(1)->length, 0u);
  auto view = buffer->reader(0);
  ASSERT_TRUE(view);
  auto term = (*view)->lookup("a");
  ASSERT_TRUE(term && *term);
  auto postings = (*term)->postings();
  ASSERT_TRUE(postings);
  EXPECT_EQ(*(*postings)->advance(0), 0u);
  EXPECT_EQ((*postings)->payload()->tf, 11u);
  auto positions = (*term)->positions(0);
  ASSERT_TRUE(positions);
  for (uint32_t pos = 0; pos < 11; ++pos) {
    EXPECT_EQ(**(*positions)->next(), pos);
  }
  EXPECT_FALSE(*(*positions)->next());
  EXPECT_EQ(*(*postings)->advance(1), 2u);
  EXPECT_EQ((*postings)->payload()->length, 2u);
  EXPECT_EQ(*(*postings)->next(), UINT32_MAX);
  EXPECT_EQ(*(*postings)->advance(0), UINT32_MAX);
  auto terms = (*source)->terms();
  ASSERT_TRUE(terms);
  for (auto key : {"a", "b"}) {
    auto head = (*terms)->next();
    ASSERT_TRUE(head && *head);
    EXPECT_EQ(*(**head).bytes(0, 1), key);
  }
  EXPECT_FALSE(*(*terms)->next());
}

TEST(ActiveBufferTest, FixedArenaCanFreezeAtCapacity) {
  auto buffer = MakeBuffer();
  ASSERT_TRUE(buffer);
  for (;;) {
    std::string key = "term-" + std::to_string(buffer->rows());
    auto doc = buffer->prepare({key, std::nullopt});
    if (!doc) {
      EXPECT_EQ(doc.error().code(), StatusCode::RESOURCE_EXHAUSTED);
      break;
    }
    buffer->apply(**doc);
  }
  EXPECT_GT(buffer->rows(), 10u);
  auto source = buffer->source(0);
  ASSERT_TRUE(source);
  const auto rows = buffer->rows();
  std::weak_ptr<FtsActiveBuffer> pin = buffer;
  buffer.reset();
  EXPECT_FALSE(pin.expired());
  EXPECT_EQ((*source)->metadata().term_count, rows);
}

TEST(ActiveBufferTest, PostingBatchesCrossWritingPartsAndRetainCursorPosition) {
  std::vector<FtsSegmentReader::Ptr> parts;
  for (uint32_t rows : {140u, 2u, 1u}) {
    auto buffer = MakeBuffer();
    ASSERT_TRUE(buffer);
    for (uint32_t row = 0; row < rows; ++row) {
      auto doc = buffer->prepare({rows == 2 ? "other" : "a a b", std::nullopt});
      ASSERT_TRUE(doc);
      buffer->apply(**doc);
    }
    auto reader = buffer->reader(0);
    ASSERT_TRUE(reader);
    parts.push_back(*reader);
  }
  auto reader = OpenWritingFtsReader(std::move(parts), {140, 2, 1});
  ASSERT_TRUE(reader && (*reader)->supports_batches());
  auto term = (*reader)->lookup("a");
  ASSERT_TRUE(term && *term);
  EXPECT_EQ((*term)->df(), 141u);
  auto cursor = (*term)->postings();
  ASSERT_TRUE(cursor);
  EXPECT_EQ(*(*cursor)->next(), 0u);
  auto batch = (*cursor)->next_batch();
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->count, FtsPostingBatch::kCapacity);
  for (uint32_t i = 0; i < batch->count; ++i) {
    EXPECT_EQ(batch->docs[i], i + 1);
    EXPECT_EQ(batch->tfs[i], 2u);
    EXPECT_EQ(batch->lengths[i], 3u);
  }
  EXPECT_EQ(*(*cursor)->advance(139), 139u);
  batch = (*cursor)->next_batch();
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->count, 1u);
  EXPECT_EQ(batch->docs[0], 142u);
  EXPECT_EQ((*cursor)->payload()->tf, 2u);
  EXPECT_EQ((*cursor)->next_batch()->count, 0u);
  EXPECT_EQ(*(*cursor)->advance(0), UINT32_MAX);
}

TEST(ActiveBufferTest, SpilledDocumentUsesExternalSortAndPreservesPositions) {
  const std::string path{"fts_active_spill.fixture"};
  ASSERT_TRUE(FileHelper::CreateDirectory(path));
  auto retirement = FtsRetirementQueue::Create();
  auto cleanup = retirement->track(path, true, true);
  auto params = std::make_shared<zvec::FtsIndexParams>(
      "whitespace", std::vector<std::string>{});
  auto schema =
      std::make_shared<FieldSchema>("body", DataType::STRING, true, params);
  std::string input;
  const std::string prefix(4800, 'x');
  // Long equal prefixes cross the comparison window. A small sort buffer
  // forces multiple passes over fixed-size records interleaved by position.
  for (int i = 0; i < 3000; ++i) {
    input += i == 5 || i == 1023 ? prefix + "z "
             : i == 6            ? prefix + "y "
                                 : (i % 2 ? "b " : "a ");
  }
  auto result = PrepareSpilledFtsDocument({schema}, {input}, path, 1,
                                          retirement, 192 * 1024, 1024 * 1024);
  ASSERT_TRUE(result) << result.error().message();
  auto source = result->front().front();
  EXPECT_EQ(source->statistics().total_tokens, 3000u);
  auto terms = source->terms();
  ASSERT_TRUE(terms);
  for (auto expected :
       {std::string("a"), std::string("b"), prefix + "y", prefix + "z"}) {
    auto term = (*terms)->next();
    ASSERT_TRUE(term && *term);
    EXPECT_EQ(*(**term).bytes(0, expected.size()), expected);
    if (expected == prefix + "z") {
      auto pos = (**term).reader->positions(0);
      ASSERT_TRUE(pos);
      EXPECT_EQ(**(*pos)->next(), 5u);
      EXPECT_EQ(**(*pos)->next(), 1023u);
      EXPECT_FALSE(*(*pos)->next());
    }
  }
  EXPECT_FALSE(*(*terms)->next());
  auto failed = PrepareSpilledFtsDocument({schema}, {input}, path, 1,
                                          retirement, 192 * 1024, 100);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code(), StatusCode::RESOURCE_EXHAUSTED);
}

TEST(ActiveBufferTest, RefusedFlushAndMergeAreRetryableAtTheHardRunLimit) {
  const std::string path{"fts_active_budget.fixture"};
  ASSERT_TRUE(FileHelper::CreateDirectory(path));
  auto retirement = FtsRetirementQueue::Create();
  auto cleanup = retirement->track(path, true, true);
  CollectionSchema schema("budget");
  auto params = std::make_shared<zvec::FtsIndexParams>(
      "whitespace", std::vector<std::string>{});
  ASSERT_TRUE(schema
                  .add_field(std::make_shared<FieldSchema>(
                      "body", DataType::STRING, true, params))
                  .ok());
  FtsWritingOptions options;
  options.active_bytes = 65536;
  options.prepare_bytes = 256 * 1024;
  options.soft_runs = options.hard_runs = 2;
  options.memory = std::make_shared<FtsWriteBudget>(options.active_bytes,
                                                    options.prepare_bytes);
  auto writer =
      FtsWritingIndex::Open(path, 1, 1, true, schema, retirement,
                            std::make_shared<FtsReadBudget>(4u << 20), options);
  ASSERT_TRUE(writer);
  auto insert = [&]() {
    auto doc = (*writer)->prepare({"token"});
    ASSERT_TRUE(doc);
    (*writer)->apply(**doc);
  };
  insert();
  {
    auto held = options.memory->reserve_job(FtsWriteBudget::Part::Flush);
    ASSERT_TRUE(held);
    EXPECT_EQ((*writer)->flush().code(), StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_EQ((*writer)->rows(), 1u);
  }
  ASSERT_TRUE((*writer)->flush().ok());
  insert();
  ASSERT_TRUE((*writer)->flush().ok());
  insert();
  {
    auto held = options.memory->reserve_job(FtsWriteBudget::Part::Merge);
    ASSERT_TRUE(held);
    EXPECT_EQ((*writer)->flush().code(), StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_EQ((*writer)->rows(), 3u);
  }
  auto root = (*writer)->checkpoint(1);
  ASSERT_TRUE(root) << root.error().message();
  EXPECT_LE(root->runs.size(), 2u);
  EXPECT_EQ(root->row_frontier, 3u);
  const auto &io = (*writer)->io_stats();
  EXPECT_GT(io.flush_spools.read_bytes, 0u);
  EXPECT_GT(io.flush_spools.written_bytes, 0u);
  EXPECT_GT(io.merge_input_bytes, 0u);
  EXPECT_GT(io.merge_spools.peak_reserved_bytes, 0u);
  EXPECT_EQ(
      (*writer)->run_file_bytes(),
      io.flush_output_bytes + io.merge_output_bytes - io.merge_input_bytes);
  EXPECT_EQ((*writer)
                ->memory_usage()
                .current[static_cast<size_t>(FtsWriteBudget::Part::Merge)],
            0u);
}

TEST(ActiveBufferTest, DirectRunIsOnlyVisibleAfterWholeDocumentApply) {
  const std::string path{"fts_active_direct.fixture"};
  ASSERT_TRUE(FileHelper::CreateDirectory(path));
  auto retirement = FtsRetirementQueue::Create();
  auto cleanup = retirement->track(path, true, true);
  CollectionSchema schema("direct");
  auto params = std::make_shared<zvec::FtsIndexParams>(
      "whitespace", std::vector<std::string>{});
  ASSERT_TRUE(schema
                  .add_field(std::make_shared<FieldSchema>(
                      "body", DataType::STRING, true, params))
                  .ok());
  FtsWritingOptions options;
  options.active_bytes = 16 * 1024;
  options.prepare_bytes = 256 * 1024;
  auto writer =
      FtsWritingIndex::Open(path, 1, 2, true, schema, retirement,
                            std::make_shared<FtsReadBudget>(4 << 20), options);
  ASSERT_TRUE(writer);
  std::string input;
  for (size_t i = 0; i < 500; ++i) {
    input += "term_" + std::to_string(i) + " ";
  }
  // This key alone exceeds both active and prepare memory. The direct run
  // feeds its dictionary in windows and remains invisible until apply.
  input.append((4u << 20) + 1, 'x');
  {
    auto pending = (*writer)->prepare({input});
    ASSERT_TRUE(pending) << pending.error().message();
    EXPECT_EQ((*writer)->rows(), 0u);
    EXPECT_FALSE((*writer)->checkpoint(1));
  }
  EXPECT_EQ((*writer)->rows(), 0u);
  auto pending = (*writer)->prepare({input});
  ASSERT_TRUE(pending);
  (*writer)->apply(**pending);
  EXPECT_EQ((*writer)->rows(), 1u);
  auto root = (*writer)->checkpoint(1);
  ASSERT_TRUE(root) << root.error().message();
  ASSERT_EQ(root->runs.size(), 1u);
  EXPECT_EQ(root->runs[0].rows, 1u);
  EXPECT_EQ(root->runs[0].parent_begin, 0u);
  auto run = FtsRun::Open(path, true, 1, 2, root->runs[0], retirement,
                          std::make_shared<FtsReadBudget>(4u << 20));
  ASSERT_TRUE(run);
  auto view = (*run)->reader(0);
  ASSERT_TRUE(view);
  auto term =
      (*view)->lookup(std::string_view(input).substr(input.rfind(' ') + 1));
  ASSERT_TRUE(term && *term);
  auto positions = (*term)->positions(0);
  ASSERT_TRUE(positions);
  EXPECT_EQ(**(*positions)->next(), 500u);
}

}  // namespace
}  // namespace zvec::fts
