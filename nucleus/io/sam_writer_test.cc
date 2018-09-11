/*
 * Copyright 2018 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "nucleus/io/sam_writer.h"

#include <utility>
#include <vector>

#include <gmock/gmock-generated-matchers.h>
#include <gmock/gmock-matchers.h>
#include <gmock/gmock-more-matchers.h>

#include "tensorflow/core/platform/test.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "nucleus/io/sam_reader.h"
#include "nucleus/testing/protocol-buffer-matchers.h"
#include "nucleus/testing/test_utils.h"
#include "nucleus/util/utils.h"
#include "nucleus/vendor/status_matchers.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/macros.h"

namespace nucleus {

using nucleus::genomics::v1::Read;
using nucleus::genomics::v1::SamReaderOptions;

namespace {

constexpr char kSamExpectedFilename[] = "expected_write.sam";
constexpr char kSamActualFilename[] = "actual_write.sam";

// Returns true if header |line| is found in table. The fields within each line
// can be arbitrarily ordered.
bool LinesMatch(absl::string_view line, const char* table[], size_t table_size,
                size_t* index) {
  std::vector<absl::string_view> tokens = absl::StrSplit(line, '\t');
  for (size_t i = 0; i < table_size; ++i) {
    absl::string_view expected_line = table[i];
    CHECK_LT(3, expected_line.length());
    if (expected_line.substr(0, 3) != tokens[0]) {
      continue;
    }
    std::vector<absl::string_view> expected_tokens =
        absl::StrSplit(expected_line, '\t');
    if (expected_tokens.size() != tokens.size()) {
      continue;
    }
    if (::testing::Value(
            tokens, ::testing::UnorderedElementsAreArray(expected_tokens))) {
      *index = i;
      return true;
    }
  }
  return false;
}

}  // namespace

class SamWriterTest : public ::testing::Test {
 protected:
  SamWriterTest()
      : expected_filename_(MakeTempFile(kSamExpectedFilename)),
        actual_filename_(MakeTempFile(kSamActualFilename)) {}
  void TearDown() override {
    // Ignore file not found errors.
    auto ignored = tensorflow::Env::Default()->DeleteFile(expected_filename_);
    ignored = tensorflow::Env::Default()->DeleteFile(actual_filename_);
  }
  const string expected_filename_;
  const string actual_filename_;
};

// Make sure writing @SQ, @RG, @PG, and @CO works.
TEST_F(SamWriterTest, WriteHeaderLines) {
  const char* kExpectedSamHeaders[] = {
      // @HD Header line.
      "@HD\tVN:1.3\tSO:coordinate\tGO:query",
      // First @SQ line.
      "@SQ\tSN:chr1\tLN:248956422",
      // Second @SQ line.
      "@SQ\tSN:KI270757.1\tLN:71251",
      // First @RG line.
      "@RG\tID:'Illumina3D.4\tPL:illumina\tLB:Illumina3D\t"
      "CN:GOOG\tDS:description\tDT:12/10/2012\tFO:ACMG\tKS:GATTACA\t"
      "PG:bwa\tPI:300\tPM:HiSeq\tPU:abcde",
      // @PG line.
      "@PG\tID:bwa\tPN:bwa\tVN:0.7.12-r1039\tCL:bwa mem -t 12 -R",
      // Second @RG line.
      "@RG\tID:'Illumina3D.4\tPL:illumina\tLB:Illumina3D\tCN:GOOG "
      "/mnt/data/ref/GRCh38.p3.genome.fa.gz Illumina3D_S6_L004_R1_001.fastq.gz "
      "Illumina3D_S6_L004_R2_001.fastq.gz",
      //  @CO line.
      "@CO\tA single line comment."};
  std::unique_ptr<tensorflow::WritableFile> expected_file;
  TF_CHECK_OK(tensorflow::Env::Default()->NewAppendableFile(expected_filename_,
                                                            &expected_file));
  for (const auto& header : kExpectedSamHeaders) {
    TF_CHECK_OK(expected_file->Append(header));
    TF_CHECK_OK(expected_file->Append("\n"));
  }
  TF_CHECK_OK(expected_file->Close());

  auto reader = std::move(
      SamReader::FromFile(expected_filename_, SamReaderOptions()).ValueOrDie());

  std::unique_ptr<SamWriter> writer = std::move(
      SamWriter::ToFile(actual_filename_, reader->Header()).ValueOrDie());
  ASSERT_THAT(writer->Close(), IsOK());

  string contents;
  TF_CHECK_OK(tensorflow::ReadFileToString(tensorflow::Env::Default(),
                                           actual_filename_, &contents));
  std::vector<absl::string_view> lines =
      absl::StrSplit(contents, '\n', absl::SkipEmpty());
  const size_t kNumHeaders = TF_ARRAYSIZE(kExpectedSamHeaders);
  ASSERT_EQ(kNumHeaders, lines.size());
  bool found[kNumHeaders] = {false};
  for (size_t i = 0; i < lines.size(); ++i) {
    size_t found_index = kNumHeaders + 1;
    if (LinesMatch(lines.at(i), kExpectedSamHeaders, kNumHeaders,
                   &found_index)) {
      ASSERT_TRUE(found_index >= 0 && found_index < kNumHeaders);
      ASSERT_FALSE(found[found_index]) << "duplicate line found";
      found[found_index] = true;
    }
  }
  for (size_t i = 0; i < kNumHeaders; ++i) {
    EXPECT_TRUE(found[i]) << "not found: " << lines.at(i);
  }
}

// Basic test to make sure writing one body line works.
TEST_F(SamWriterTest, WriteOneBodyLine) {
  const char kExpectedSamHeader[] = "@SQ\tSN:chr1\tLN:248956422";
  const std::vector<const char*> kExpectedSamContent = {
      "NS500473:5:H17BCBGXX:4:11609:2859:12884",  // QNAME
      "99",                                       // FLAG
      "chr1",                                     // RNAME
      "10034",                                    // POS
      "0",                                        // MAPQ
      "29M6S",                                    // CIGAR
      "=",                                        // RNEXT
      "10354",                                    // PNEXT
      "348",                                      // TLEN
      "CCCTAACCCTAACCCTAACCCTAACCCTANNNNNN",      // SEQ
      "AAA7<<7FAFA..FFFF7FFFF))F<FFF######"};     // QUAL
  std::unique_ptr<tensorflow::WritableFile> expected_file;
  TF_CHECK_OK(tensorflow::Env::Default()->NewAppendableFile(expected_filename_,
                                                            &expected_file));
  TF_CHECK_OK(expected_file->Append(kExpectedSamHeader));
  TF_CHECK_OK(expected_file->Append("\n"));
  TF_CHECK_OK(expected_file->Append(absl::StrJoin(
      kExpectedSamContent.begin(), kExpectedSamContent.end(), "\t")));
  TF_CHECK_OK(expected_file->Close());

  auto reader = std::move(
      SamReader::FromFile(expected_filename_, SamReaderOptions()).ValueOrDie());
  std::vector<Read> reads = as_vector(reader->Iterate());

  std::unique_ptr<SamWriter> writer = std::move(
      SamWriter::ToFile(actual_filename_, reader->Header()).ValueOrDie());
  for (const nucleus::genomics::v1::Read& r : reads) {
    EXPECT_THAT(writer->Write(r), IsOK());
  }
  ASSERT_THAT(writer->Close(), IsOK());

  string contents;
  TF_CHECK_OK(tensorflow::ReadFileToString(tensorflow::Env::Default(),
                                           actual_filename_, &contents));
  std::vector<absl::string_view> lines = absl::StrSplit(contents, '\n');
  ASSERT_EQ(4u, lines.size());
  EXPECT_EQ("@HD\tSO:unknown\tGO:none", lines.at(0));
  EXPECT_EQ(kExpectedSamHeader, lines.at(1));
  std::vector<absl::string_view> fields = absl::StrSplit(lines.at(2), '\t');
  EXPECT_EQ(kExpectedSamContent.size(), fields.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    EXPECT_EQ(kExpectedSamContent[i], fields[i]);
  }
  EXPECT_TRUE(lines.at(3).empty());
}

// Test SAM, BAM, CRAM formats.
class SamBamWriterTest : public SamWriterTest,
                         public ::testing::WithParamInterface<string> {};

INSTANTIATE_TEST_CASE_P(/* no prefix */, SamBamWriterTest,
                        ::testing::Values("test_cram.cram", "test.sam",
                                          "test.bam"));

TEST_P(SamBamWriterTest, WriteAndThenRead) {
  auto options = SamReaderOptions();
  options.set_aux_field_handling(SamReaderOptions::SKIP_AUX_FIELDS);
  string ref_path;
  if (GetParam() == "test_cram.cram") {
    ref_path = GetTestData("test.fasta");
  }
  // Read from the original file.
  auto reader =
      std::move(SamReader::FromFile(GetTestData(GetParam()), ref_path, options)
                    .ValueOrDie());
  std::vector<Read> reads = as_vector(reader->Iterate());
  ASSERT_THAT(reader->Close(), IsOK());

  const string actual_filename = MakeTempFile(GetParam());
  std::unique_ptr<SamWriter> writer =
      std::move(SamWriter::ToFile(actual_filename, ref_path, reader->Header())
                    .ValueOrDie());
  for (const nucleus::genomics::v1::Read& r : reads) {
    EXPECT_THAT(writer->Write(r), IsOK());
  }
  ASSERT_THAT(writer->Close(), IsOK());

  // Now read from the written file. The reads should match that of the original
  // file.
  auto reader2 = std::move(
      SamReader::FromFile(actual_filename, ref_path, SamReaderOptions())
          .ValueOrDie());
  std::vector<Read> reads2 = as_vector(reader2->Iterate());
  ASSERT_THAT(reader2->Close(), IsOK());

  ASSERT_EQ(reads.size(), reads2.size());
  for (size_t i = 0; i < reads.size(); ++i) {
    EXPECT_THAT(reads[i], EqualsProto(reads2[i]));
  }
  TF_CHECK_OK(tensorflow::Env::Default()->DeleteFile(actual_filename));
}

}  // namespace nucleus
