/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEY_SEARCH_INDEXES_TEXT_ARABIC_NORMALIZER_H_
#define VALKEY_SEARCH_INDEXES_TEXT_ARABIC_NORMALIZER_H_

#include <string>

#include "src/indexes/text/language_processor.h"

namespace valkey_search::indexes::text {

/// TokenFilter that applies Arabic-specific normalization steps.
///
/// This filter runs AFTER Unicode NFKC normalization + case folding, and
/// BEFORE stop word filtering, to ensure consistent Arabic text matching.
///
/// Normalization steps performed:
///   1. Strip tashkeel diacritics (U+064B–U+065F, U+0670)
///   2. Remove tatweel / kashida (U+0640)
///   3. Normalize alef variants (أ إ آ ٱ → ا)
///   4. Normalize teh marbuta (ة → ه)
///   5. Normalize alef maksura (ى → ي)
///
/// These normalizations match what the Snowball Arabic stemmer expects as
/// input, and align with standard Arabic IR normalization practices (the
/// "light" normalization used by Lucene's ArabicNormalizer).
///
/// Always returns true (keeps all tokens).
class ArabicNormalizationFilter : public TokenFilter {
 public:
  ArabicNormalizationFilter() = default;

  bool Apply(std::string &token) const override;

 private:
  /// Normalize a single UTF-8 token in place.
  /// Walks code points, stripping/replacing as needed, and writes the result
  /// back into the same string.
  static void NormalizeArabicInPlace(std::string &token);
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_ARABIC_NORMALIZER_H_
