/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/text/chinese_processor.h"

#include <utility>

#include "src/indexes/text/jieba_dict_trie.h"
#include "src/indexes/text/jieba_segmenter.h"
#include "src/indexes/text/normalize_case_fold_filter.h"
#include "src/indexes/text/segmenter_query_tokenizer.h"
#include "src/indexes/text/stop_word_filter.h"
#include "src/indexes/text/stop_words.h"
#include "src/indexes/text/unicode_normalizer.h"

// Dictionary path — set at build time via CMake's CPPJIEBA_DICT_DIR.
// This points to the byronhe fork's dict/ directory containing:
//   jieba.dict.utf8, hmm_model.utf8, user.dict.utf8, idf.utf8, stop_words.utf8
#ifndef CPPJIEBA_DICT_DIR
#define CPPJIEBA_DICT_DIR "/usr/share/valkey-search/jieba-dict"
#endif

namespace valkey_search::indexes::text {

std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::vector<std::string>& stop_words) {
  auto processor = std::make_shared<LanguageProcessor>();

  // Load or reuse the shared dictionary (process-wide singleton, ~60 MB).
  static const std::string dict_dir = CPPJIEBA_DICT_DIR;
  auto dictionary = JiebaDictionary::GetInstance(
      dict_dir + "/jieba.dict.utf8", dict_dir + "/hmm_model.utf8",
      dict_dir + "/user.dict.utf8", dict_dir + "/idf.utf8",
      dict_dir + "/stop_words.utf8");

  // Segmenter: CutForSearch mode for ingestion (overlapping compound tokens).
  // Uses shared DictTrie via const pointer — no per-instance copy.
  // cppjieba's Cut methods are fully const (Viterbi buffers are stack-local),
  // so this single instance is safe to share between the ingestion pipeline
  // and the query tokenizer.
  auto segmenter = std::make_shared<JiebaSegmenter>(dictionary);
  processor->segmenters_.push_back(segmenter);

  // Query tokenizer: delegates to the same JiebaSegmenter for CJK word
  // boundary detection within query text spans.
  processor->query_tokenizer_ =
      std::make_shared<SegmenterQueryTokenizer>(segmenter);

  // Filter 1: NFKC normalization + Unicode case folding.
  // NFKC is preferred for CJK — it normalizes fullwidth characters
  // (e.g., Ａ → A, ０ → 0) and compatibility mappings.
  auto normalizer =
      std::make_shared<NormalizeCaseFoldFilter>(NormalizationForm::NFKC);
  processor->normalizer_ = normalizer;
  processor->filters_.push_back(std::move(normalizer));

  // Filter 2: Stop word removal.
  // Use provided stop words if non-empty, else fall back to defaults.
  const auto& effective_stops =
      stop_words.empty() ? kChineseStopWords : stop_words;
  auto stop_filter = std::make_shared<StopWordFilter>(effective_stops);
  processor->stop_word_filter_ = stop_filter;
  processor->filters_.push_back(std::move(stop_filter));

  // No stemmer for Chinese — Chinese does not have inflectional morphology.
  processor->stemmer_ = nullptr;

  return processor;
}

}  // namespace valkey_search::indexes::text
