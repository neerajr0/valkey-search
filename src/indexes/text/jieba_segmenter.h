/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEY_SEARCH_INDEXES_TEXT_JIEBA_SEGMENTER_H_
#define VALKEY_SEARCH_INDEXES_TEXT_JIEBA_SEGMENTER_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "cppjieba/QuerySegment.hpp"
#include "src/indexes/text/jieba_dict_trie.h"
#include "src/indexes/text/segmenter.h"

namespace valkey_search::indexes::text {

/// Chinese word segmenter backed by cppjieba.
///
/// Thread-safety model:
///   - The JiebaDictionary (DictTrie + HMMModel) is shared read-only
///     across all instances (~60 MB total, loaded once).
///   - Each JiebaSegmenter instance owns a QuerySegment that holds only
///     lightweight const pointers to the shared DictTrie/HMMModel.
///   - cppjieba's Cut/CutToStr methods are fully const — all working
///     buffers (Viterbi path/weight vectors) are stack-local per call.
///   - Therefore, a single JiebaSegmenter instance can be safely shared
///     across threads for concurrent read-only Segment() calls.
///
/// Per-instance memory: negligible (two const pointers + symbol set).
/// Per-call memory: Viterbi buffers allocated on the stack/heap per call,
/// proportional to input length.
class JiebaSegmenter : public Segmenter {
 public:
  /// Construct a segmenter with an externally-provided shared dictionary.
  /// The QuerySegment instance references the shared DictTrie directly —
  /// no per-instance dictionary copy.
  explicit JiebaSegmenter(std::shared_ptr<JiebaDictionary> dict);

  absl::StatusOr<std::vector<std::string>> Segment(
      absl::string_view text) const override;

 private:
  std::shared_ptr<JiebaDictionary> dict_;
  // QuerySegment uses MixSegment internally and adds sub-word expansion
  // for tokens longer than 2 characters. It accepts const DictTrie* and
  // const HMMModel* — no ownership, no copy.
  std::unique_ptr<cppjieba::QuerySegment> query_segment_;
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_JIEBA_SEGMENTER_H_
