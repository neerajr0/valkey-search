/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/text/jieba_segmenter.h"

#include <string>
#include <utility>
#include <vector>

namespace valkey_search::indexes::text {

JiebaSegmenter::JiebaSegmenter(std::shared_ptr<JiebaDictionary> dict)
    : dict_(std::move(dict)) {
  // Construct QuerySegment using const pointers to the shared DictTrie and
  // HMMModel. These are read-only references — no per-instance dictionary
  // copy. cppjieba's Cut/CutToStr methods are fully const with all working
  // buffers (Viterbi path/weight vectors) allocated per-call on the stack.
  query_segment_ = std::make_unique<cppjieba::QuerySegment>(
      &dict_->GetDictTrie(), &dict_->GetHMMModel());
}

absl::StatusOr<std::vector<std::string>> JiebaSegmenter::Segment(
    absl::string_view text) const {
  if (text.empty()) {
    return std::vector<std::string>{};
  }

  std::string input(text);
  std::vector<std::string> tokens;

  // CutForSearch (QuerySegment): MixSegment first, then for tokens longer
  // than 2 characters, searches the DictTrie for valid sub-word bigrams
  // and trigrams and emits them as additional overlapping tokens.
  // Example: "量子计算" → ["量子", "计算", "量子计算"]
  query_segment_->CutToStr(input, tokens, /*hmm=*/true);

  return tokens;
}

}  // namespace valkey_search::indexes::text
