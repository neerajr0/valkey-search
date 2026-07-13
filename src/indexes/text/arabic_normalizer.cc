/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/text/arabic_normalizer.h"

#include <cstdint>
#include <string>

#include "src/utils/scanner.h"

namespace valkey_search::indexes::text {

namespace {

// ---------------------------------------------------------------------------
// Arabic Unicode code point constants
// ---------------------------------------------------------------------------

// Tashkeel (diacritics) — stripped entirely
constexpr uint32_t kFathatan = 0x064B;
constexpr uint32_t kDammatan = 0x064C;
constexpr uint32_t kKasratan = 0x064D;
constexpr uint32_t kFatha = 0x064E;
constexpr uint32_t kDamma = 0x064F;
constexpr uint32_t kKasra = 0x0650;
constexpr uint32_t kShadda = 0x0651;
constexpr uint32_t kSukun = 0x0652;
constexpr uint32_t kMaddaAbove = 0x0653;
constexpr uint32_t kHamzaAbove = 0x0654;
constexpr uint32_t kHamzaBelow = 0x0655;
constexpr uint32_t kSubscriptAlef = 0x0656;
constexpr uint32_t kInvertedDamma = 0x0657;
constexpr uint32_t kMarkNunGhunna = 0x0658;
constexpr uint32_t kZwarakay = 0x0659;  // Pashto
// Additional tashkeel range: U+065A–U+065F (various Quranic marks)
constexpr uint32_t kTashkeelRangeEnd = 0x065F;
// Superscript alef (U+0670) — diacritic-like, stripped
constexpr uint32_t kSuperscriptAlef = 0x0670;

// Tatweel (kashida) — stripped
constexpr uint32_t kTatweel = 0x0640;

// Alef variants — normalized to bare alef (U+0627)
constexpr uint32_t kAlef = 0x0627;            // ا - target
constexpr uint32_t kAlefMadda = 0x0622;       // آ
constexpr uint32_t kAlefHamzaAbove = 0x0623;  // أ
constexpr uint32_t kAlefHamzaBelow = 0x0625;  // إ
constexpr uint32_t kAlefWasla = 0x0671;       // ٱ

// Teh marbuta (ة) → heh (ه)
constexpr uint32_t kTehMarbuta = 0x0629;
constexpr uint32_t kHeh = 0x0647;

// Alef maksura (ى) → yeh (ي)
constexpr uint32_t kAlefMaksura = 0x0649;
constexpr uint32_t kYeh = 0x064A;

/// Returns true if the code point is a tashkeel (Arabic diacritic) that
/// should be stripped for IR normalization.
inline bool IsTashkeel(uint32_t cp) {
  return (cp >= kFathatan && cp <= kTashkeelRangeEnd) || cp == kSuperscriptAlef;
}

}  // namespace

void ArabicNormalizationFilter::NormalizeArabicInPlace(std::string &token) {
  std::string result;
  result.reserve(token.size());  // At most same size (stripping only shrinks)

  size_t pos = 0;
  while (pos < token.size()) {
    utils::Scanner s(token.substr(pos));
    uint32_t cp = s.NextUtf8();

    if (cp == utils::Scanner::kEOF) {
      break;
    }
    if (cp == utils::Scanner::kInvalidCp) {
      // Pass through invalid bytes unchanged (shouldn't happen after earlier
      // validation, but be defensive)
      result.push_back(token[pos]);
      pos++;
      continue;
    }

    uint8_t len = s.LastUtf8ByteLen();
    pos += len;

    // 1. Strip tashkeel diacritics
    if (IsTashkeel(cp)) {
      continue;
    }

    // 2. Strip tatweel
    if (cp == kTatweel) {
      continue;
    }

    // 3. Normalize alef variants → bare alef
    if (cp == kAlefMadda || cp == kAlefHamzaAbove || cp == kAlefHamzaBelow ||
        cp == kAlefWasla) {
      utils::Scanner::PushBackUtf8(result, kAlef);
      continue;
    }

    // 4. Normalize teh marbuta → heh
    if (cp == kTehMarbuta) {
      utils::Scanner::PushBackUtf8(result, kHeh);
      continue;
    }

    // 5. Normalize alef maksura → yeh
    if (cp == kAlefMaksura) {
      utils::Scanner::PushBackUtf8(result, kYeh);
      continue;
    }

    // Default: keep the code point as-is
    result.append(token.data() + pos - len, len);
  }

  token = std::move(result);
}

bool ArabicNormalizationFilter::Apply(std::string &token) const {
  NormalizeArabicInPlace(token);
  // If normalization stripped all characters (e.g., a standalone tatweel or
  // tashkeel-only token), drop it. Passing an empty string downstream to the
  // Snowball stemmer causes a crash (zero-length assertion / buffer underflow).
  return !token.empty();
}

}  // namespace valkey_search::indexes::text
