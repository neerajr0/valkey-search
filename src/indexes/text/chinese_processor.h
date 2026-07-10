/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEY_SEARCH_INDEXES_TEXT_CHINESE_PROCESSOR_H_
#define VALKEY_SEARCH_INDEXES_TEXT_CHINESE_PROCESSOR_H_

#include <memory>
#include <string>
#include <vector>

#include "src/indexes/text/language_processor.h"

namespace valkey_search::indexes::text {

/// Create a LanguageProcessor composed for Chinese.
///
/// Composition:
///   Segmenter:      JiebaSegmenter (CutForSearch mode, shared DictTrie)
///   QueryTokenizer: SegmenterQueryTokenizer (delegates to JiebaSegmenter)
///   Filters:        NormalizeCaseFoldFilter(NFKC) → StopWordFilter
///   Stemmer:        nullptr (Chinese does not use stemming)
///
/// Memory model:
///   - DictTrie + HMMModel: ~60 MB shared across all Chinese processors
///   - Per-processor overhead: ~60 KB (Viterbi decode buffers)
///
/// The factory loads the dictionary lazily on first call. Subsequent calls
/// reuse the same shared_ptr<JiebaDictionary>.
std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::vector<std::string>& stop_words);

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_CHINESE_PROCESSOR_H_
