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

#include <gtest/gtest.h>
#include "db/common/file_helper.h"
#include "db/index/common/manifest/pb_wire.h"
#include "db/index/common/manifest_codec.h"
#include "db/index/common/version_manager.h"

namespace zvec {
namespace {

ManifestData CustomManifest() {
  ManifestData data;
  data.schema = std::make_shared<CollectionSchema>();
  auto segment = std::make_shared<SegmentMeta>(3);
  BlockMeta block{7, BlockType::FTS_INDEX, 10, 19, 10, {"text"}};
  block.fts_base_.emplace();
  block.fts_base_->generation = 5;
  block.fts_base_->row_count = 10;
  block.fts_base_->base_id[0] = 11;
  block.fts_base_->metadata_digest[0] = 12;
  segment->add_persisted_block(block);
  data.persisted_segment_metas.push_back(segment);
  return data;
}

TEST(FtsManifestTest, CustomBaseRoundTripsBehindOldReaderGate) {
  const auto source = CustomManifest();
  std::string bytes;
  ASSERT_TRUE(ManifestCodec::Encode(source, &bytes).ok());
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ('\0', bytes.front());  // Illegal protobuf field zero.
  pbwire::Reader old_wire(bytes);
  EXPECT_FALSE(old_wire.next());
  EXPECT_FALSE(old_wire.ok());
  ManifestData decoded;
  ASSERT_TRUE(ManifestCodec::Decode(bytes, &decoded).ok());
  ASSERT_EQ(1u, decoded.persisted_segment_metas.size());
  EXPECT_EQ(*source.persisted_segment_metas[0],
            *decoded.persisted_segment_metas[0]);
}

TEST(FtsManifestTest, CustomBaseCannotHideInUngatedLegacyWire) {
  const auto source = CustomManifest();
  std::string segment;
  ManifestCodec::EncodeSegmentMeta(*source.persisted_segment_metas[0],
                                   &segment);
  std::string bytes;
  pbwire::Writer writer(&bytes);
  writer.put_message(4, segment);  // Existing persisted-segment field.
  ManifestData decoded;
  EXPECT_EQ(StatusCode::NOT_SUPPORTED,
            ManifestCodec::Decode(bytes, &decoded).code());
}

TEST(FtsManifestTest, LegacyManifestBytesRemainUnchanged) {
  ManifestData source;
  source.schema = std::make_shared<CollectionSchema>();
  std::string bytes;
  ASSERT_TRUE(ManifestCodec::Encode(source, &bytes).ok());
  const char golden[] = {0x12,       0x05,       0x18, char(0x80),
                         char(0xad), char(0xe2), 0x04};
  EXPECT_EQ(std::string(golden, sizeof(golden)), bytes);
}

TEST(FtsManifestTest, CorruptEnvelopeDoesNotPublishPartialMetadata) {
  std::string bytes;
  ASSERT_TRUE(ManifestCodec::Encode(CustomManifest(), &bytes).ok());
  ManifestData decoded;
  decoded.version = 99;
  // Every truncation of a gated manifest must fail, including the envelope.
  for (size_t cut = 1; cut < bytes.size(); ++cut) {
    EXPECT_FALSE(
        ManifestCodec::Decode(std::string_view(bytes).substr(0, cut), &decoded)
            .ok())
        << cut;
    EXPECT_EQ(99u, decoded.version);
  }
  for (size_t pos = 1; pos < bytes.size(); ++pos) {
    auto corrupt = bytes;
    corrupt[pos] ^= 1;
    EXPECT_FALSE(ManifestCodec::Decode(corrupt, &decoded).ok()) << pos;
    EXPECT_EQ(99u, decoded.version);
  }
  EXPECT_FALSE(ManifestCodec::Decode(bytes + "trailing", &decoded).ok());
}

TEST(FtsManifestTest, UnsupportedBasesAndMalformedDescriptorsFailClosed) {
  auto data = CustomManifest();
  const auto original = data.persisted_segment_metas[0]->persisted_blocks()[0];
  for (unsigned variant = 0; variant < 7; ++variant) {
    auto block = original;
    switch (variant) {
      case 0:
        block.fts_base_->format_version = 2;
        break;
      case 1:
        block.fts_base_->minimum_reader_version = 2;
        break;
      case 2:
        block.fts_base_->required_features = 1;
        break;
      case 3:
        block.fts_base_->base_id.fill(0);
        break;
      case 4:
        block.fts_base_->generation = 0;
        break;
      case 5:
        block.fts_base_->row_count = 11;
        break;
      case 6:
        block.set_type(BlockType::SCALAR);
        break;
    }
    const auto expected =
        variant < 3 ? StatusCode::NOT_SUPPORTED : StatusCode::INTERNAL_ERROR;
    std::string bytes;
    ManifestCodec::EncodeBlockMeta(block, &bytes);
    Status error;
    EXPECT_EQ(nullptr, ManifestCodec::DecodeBlockMeta(bytes, &error));
    EXPECT_EQ(expected, error.code());
    data.persisted_segment_metas[0]->persisted_blocks()[0] = block;
    bytes = "untouched";
    EXPECT_EQ(expected, ManifestCodec::Encode(data, &bytes).code());
    EXPECT_EQ("untouched", bytes);
  }
  std::string bytes;
  ManifestCodec::EncodeBlockMeta(original, &bytes);
  pbwire::Writer writer(&bytes);
  writer.put_message(7, "short duplicate");
  Status error;
  EXPECT_EQ(nullptr, ManifestCodec::DecodeBlockMeta(bytes, &error));
  EXPECT_FALSE(error.ok());
}

TEST(FtsManifestTest, OptionalBlockEntriesRemainSkippable) {
  auto data = CustomManifest();
  const auto &original = data.persisted_segment_metas[0]->persisted_blocks()[0];
  std::string bytes;
  ManifestCodec::EncodeBlockMeta(original, &bytes);
  pbwire::Writer writer(&bytes);
  writer.put_message(42, "future optional resource description");
  Status error;
  auto block = ManifestCodec::DecodeBlockMeta(bytes, &error);
  ASSERT_TRUE(error.ok());
  ASSERT_NE(nullptr, block);
  EXPECT_EQ(original, *block);
}

TEST(FtsManifestTest, VersionLoadPreservesUnsupportedFormatError) {
  const std::string path = "unsupported_manifest.fixture";
  ailego::File file;
  ASSERT_TRUE(file.create(path, 0));
  const std::string bytes("\0ZVEC_FTS_MANIFEST_V3\0", 22);
  ASSERT_EQ(bytes.size(), file.write(bytes.data(), bytes.size()));
  file.close();
  Version version;
  EXPECT_EQ(StatusCode::NOT_SUPPORTED, Version::Load(path, &version).code());
  EXPECT_TRUE(FileHelper::RemoveFile(path));
}

TEST(FtsManifestTest, WritingRootRequiresV2AndAnExactAdjacentCover) {
  ManifestData data;
  data.schema = std::make_shared<CollectionSchema>("writing");
  data.writing_segment_meta = std::make_shared<SegmentMeta>(7);
  data.writing_segment_meta->set_writing_forward_block(
      {5, BlockType::SCALAR, 3, 0, 0, {"text"}});
  BlockMeta block(4, BlockType::FTS_INDEX, 0, 2, 3, {"text"});
  auto &root = block.fts_writing_.emplace();
  root.wal_epoch = 17;
  root.row_frontier = 3;
  root.writer_id[0] = 5;
  root.schema_identity = ManifestCodec::SchemaIdentity(*data.schema);
  FtsRunDescriptor run;
  run.id[0] = 1;
  run.metadata_digest[0] = 2;
  run.generation = 1;
  run.file_size = 8192;
  run.rows = 2;
  root.runs.push_back(run);
  run.id[0] = 3;
  run.parent_begin = 2;
  run.rows = 1;
  root.runs.push_back(run);
  data.writing_segment_meta->add_persisted_block(block);
  data.writing_segment_meta->add_persisted_block(
      {3, BlockType::SCALAR, 0, 2, 3, {"text"}});
  std::string bytes;
  ASSERT_TRUE(ManifestCodec::Encode(data, &bytes).ok());
  const std::string prefix("\0ZVEC_FTS_MANIFEST_V2\0", 22);
  EXPECT_EQ(prefix, bytes.substr(0, prefix.size()));
  // Golden SHA-256 of the complete, independently encoded role + protobuf
  // payload, including the fixed-width identities and the two run ranges.
  constexpr uint8_t golden[32] = {
      0xb2, 0xdc, 0x46, 0xe7, 0x1a, 0x98, 0x05, 0x8c, 0xa9, 0xbc, 0xe2,
      0xa4, 0xc2, 0x9e, 0xed, 0xe3, 0x5b, 0x00, 0x8e, 0xed, 0xe7, 0x2b,
      0xa0, 0xb1, 0x29, 0xe1, 0x9d, 0xc2, 0x3a, 0x44, 0x39, 0xa2};
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(golden), sizeof(golden)),
            bytes.substr(prefix.size() + sizeof(uint64_t), sizeof(golden)));
  ManifestData decoded;
  ASSERT_TRUE(ManifestCodec::Decode(bytes, &decoded).ok());
  EXPECT_TRUE(decoded.use_fts_runs);
  EXPECT_TRUE(decoded.writing_protocol);
  ASSERT_TRUE(decoded.writing_segment_meta->writing_fts_checkpoint());
  EXPECT_EQ(root, *decoded.writing_segment_meta->writing_fts_checkpoint());
  auto &persisted = data.writing_segment_meta->persisted_blocks()[0];
  for (int failure = 0; failure < 6; ++failure) {
    persisted = block;
    auto &bad = *persisted.fts_writing_;
    switch (failure) {
      case 0:
        bad.runs.back().parent_begin = 1;
        break;
      case 1:
        bad.runs.back().id = bad.runs.front().id;
        break;
      case 2:
        ++bad.runs.back().rows;
        break;
      case 3:
        bad.runs.resize(17);
        break;
      case 4:
        bad.schema_identity.fill(0xff);
        break;
      case 5:
        persisted.fts_base_.emplace();
        break;
    }
    bytes.clear();
    EXPECT_FALSE(ManifestCodec::Encode(data, &bytes).ok()) << failure;
    EXPECT_TRUE(bytes.empty());
  }
  persisted = block;
  persisted.set_doc_count(0);
  persisted.fts_writing_->row_frontier = 0;
  persisted.fts_writing_->runs.clear();
  data.writing_segment_meta->persisted_blocks()[1].set_doc_count(0);
  EXPECT_TRUE(ManifestCodec::Encode(data, &bytes).ok());
}

}  // namespace
}  // namespace zvec
