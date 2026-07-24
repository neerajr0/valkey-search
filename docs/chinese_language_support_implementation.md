# Chinese Language Support — Implementation Plan

## Summary

This document details all low-level code changes required to integrate Chinese (cppjieba) support into valkey-search using the existing pluggable `LanguageProcessor` pipeline. The design follows the "add and plug, don't rework" principle — all changes are additive to the existing architecture.

---

## 1. Proto Schema Change

### File: `src/index_schema.proto`

Add `LANGUAGE_CHINESE` to the `Language` enum:

```proto
enum Language {
  LANGUAGE_UNSPECIFIED = 0;
  LANGUAGE_ENGLISH = 1;
  // ... existing languages ...
  LANGUAGE_ARABIC = 12;
  LANGUAGE_CHINESE = 13;  // NEW
}
```

**Impact**: Regenerates `index_schema.pb.h` / `index_schema.pb.cc`. All downstream code referencing `data_model::LANGUAGE_CHINESE` will compile.

---

## 2. Third-Party Dependency: cppjieba (byronhe's fork)

### New file: `third_party/cppjieba/CMakeLists.txt`

```cmake
# cppjieba — Header-only Chinese segmentation (MIT license)
# Uses byronhe's fork with reduced dictionary (~5 MB on disk → ~60 MB RSS
# vs upstream's 11 MB on disk → ~126 MB RSS).

include(FetchContent)

FetchContent_Declare(
  cppjieba
  GIT_REPOSITORY https://github.com/byronhe/cppjieba.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

FetchContent_GetProperties(cppjieba)
if(NOT cppjieba_POPULATED)
  FetchContent_Populate(cppjieba)
endif()

add_library(cppjieba INTERFACE)
target_include_directories(cppjieba INTERFACE
  ${cppjieba_SOURCE_DIR}/include
  ${cppjieba_SOURCE_DIR}/deps/limonp/include
)

# Export dictionary path so the module can locate dict files at runtime
set(CPPJIEBA_DICT_DIR "${cppjieba_SOURCE_DIR}/dict"
    CACHE PATH "Path to cppjieba dictionary files" FORCE)
```

### Modified file: `third_party/CMakeLists.txt`

Add at end:

```cmake
add_subdirectory(cppjieba)
```

### Modified file: `src/indexes/CMakeLists.txt`

Add to the `text` library's link dependencies:

```cmake
target_link_libraries(text PUBLIC cppjieba)
```

---

## 3. Shared DictTrie — Thread-Safe Singleton

### New file: `src/indexes/text/jieba_dict_trie.h`

**Purpose**: Extract cppjieba's `DictTrie` into a standalone `std::shared_ptr`-managed object that all threads share. This eliminates the 60 MB-per-thread duplication.

```cpp
#ifndef VALKEY_SEARCH_INDEXES_TEXT_JIEBA_DICT_TRIE_H_
#define VALKEY_SEARCH_INDEXES_TEXT_JIEBA_DICT_TRIE_H_

#include <memory>
#include <mutex>
#include <string>

#include "cppjieba/DictTrie.hpp"
#include "cppjieba/HMMModel.hpp"

namespace valkey_search::indexes::text {

/// Lazily-initialized, process-wide shared dictionary for cppjieba.
///
/// The DictTrie + HMMModel are read-only after construction, making them
/// safe to share across threads without synchronization on the read path.
/// Only initialization is guarded by a mutex (std::call_once).
///
/// Memory budget (byronhe fork):
///   - DictTrie:  ~50 MB RSS (DAT trie from reduced dict)
///   - HMMModel:  ~10 MB RSS (transition/emission matrices)
///   - Total:     ~60 MB shared across all threads
class JiebaDictionary {
 public:
  /// Get the process-wide singleton. First call triggers dictionary load.
  /// Thread-safe via std::call_once.
  static std::shared_ptr<JiebaDictionary> GetInstance(
      const std::string& dict_path,
      const std::string& hmm_model_path,
      const std::string& user_dict_path,
      const std::string& idf_path,
      const std::string& stop_word_path);

  /// Access the shared DictTrie (read-only after init).
  const cppjieba::DictTrie& GetDictTrie() const { return *dict_trie_; }

  /// Access the shared HMM model (read-only after init).
  const cppjieba::HMMModel& GetHMMModel() const { return *hmm_model_; }

  /// Get dictionary file paths for callers that need them.
  const std::string& GetDictPath() const { return dict_path_; }
  const std::string& GetHMMModelPath() const { return hmm_model_path_; }
  const std::string& GetUserDictPath() const { return user_dict_path_; }
  const std::string& GetIdfPath() const { return idf_path_; }
  const std::string& GetStopWordPath() const { return stop_word_path_; }

 private:
  JiebaDictionary(const std::string& dict_path,
                  const std::string& hmm_model_path,
                  const std::string& user_dict_path,
                  const std::string& idf_path,
                  const std::string& stop_word_path);

  std::unique_ptr<cppjieba::DictTrie> dict_trie_;
  std::unique_ptr<cppjieba::HMMModel> hmm_model_;
  std::string dict_path_;
  std::string hmm_model_path_;
  std::string user_dict_path_;
  std::string idf_path_;
  std::string stop_word_path_;

  static std::shared_ptr<JiebaDictionary> instance_;
  static std::once_flag init_flag_;
};

}  // namespace valkey_search::indexes::text

#endif
```

### New file: `src/indexes/text/jieba_dict_trie.cc`

```cpp
#include "src/indexes/text/jieba_dict_trie.h"

namespace valkey_search::indexes::text {

std::shared_ptr<JiebaDictionary> JiebaDictionary::instance_;
std::once_flag JiebaDictionary::init_flag_;

std::shared_ptr<JiebaDictionary> JiebaDictionary::GetInstance(
    const std::string& dict_path,
    const std::string& hmm_model_path,
    const std::string& user_dict_path,
    const std::string& idf_path,
    const std::string& stop_word_path) {
  std::call_once(init_flag_, [&]() {
    instance_ = std::shared_ptr<JiebaDictionary>(
        new JiebaDictionary(dict_path, hmm_model_path, user_dict_path,
                            idf_path, stop_word_path));
  });
  return instance_;
}

JiebaDictionary::JiebaDictionary(const std::string& dict_path,
                                 const std::string& hmm_model_path,
                                 const std::string& user_dict_path,
                                 const std::string& idf_path,
                                 const std::string& stop_word_path)
    : dict_path_(dict_path),
      hmm_model_path_(hmm_model_path),
      user_dict_path_(user_dict_path),
      idf_path_(idf_path),
      stop_word_path_(stop_word_path) {
  dict_trie_ = std::make_unique<cppjieba::DictTrie>(dict_path, user_dict_path);
  hmm_model_ = std::make_unique<cppjieba::HMMModel>(hmm_model_path);
}

}  // namespace valkey_search::indexes::text
```

---

## 4. Jieba Segmenter — Per-Thread Lightweight State

### New file: `src/indexes/text/jieba_segmenter.h`

**Purpose**: Implements the `Segmenter` interface. Holds a `shared_ptr<JiebaDictionary>` (shared, ~60 MB) and per-instance decode buffers (~60 KB) for HMM Viterbi and segment result state. Supports both `CutForSearch` (ingestion) and `QuerySegment` (query) modes.

```cpp
#ifndef VALKEY_SEARCH_INDEXES_TEXT_JIEBA_SEGMENTER_H_
#define VALKEY_SEARCH_INDEXES_TEXT_JIEBA_SEGMENTER_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/indexes/text/jieba_dict_trie.h"
#include "src/indexes/text/segmenter.h"

namespace valkey_search::indexes::text {

/// Segmentation mode for the Jieba segmenter.
enum class JiebaMode {
  /// CutForSearch: Produces overlapping tokens for compound words.
  /// "量子计算" → ["量子", "计算", "量子计算"]
  /// Used at ingestion time to maximize recall.
  kCutForSearch,

  /// QuerySegment: Uses keyword extraction weighting to produce
  /// query-optimized tokens. Shorter output, focused on high-IDF terms.
  /// Used at query time for precision.
  kQuerySegment,
};

/// Chinese word segmenter backed by cppjieba.
///
/// Thread-safety model:
///   - The JiebaDictionary (DictTrie + HMMModel) is shared read-only
///     across all instances (~60 MB total, loaded once).
///   - Each JiebaSegmenter instance owns lightweight decode buffers
///     (~60 KB) for HMM Viterbi state and segment result vectors.
///   - Instances are NOT shared across threads — each LanguageProcessor
///     (and therefore each thread's processing context) gets its own.
///
/// This gives O(1) per-thread overhead while sharing the expensive
/// dictionary data structure.
class JiebaSegmenter : public Segmenter {
 public:
  /// Construct a segmenter with an externally-provided shared dictionary.
  /// This is the preferred constructor for production use.
  /// The MixSegment/HMMSegment instances reference the shared DictTrie
  /// directly — no per-instance dictionary copy, only decode buffers (~60 KB).
  explicit JiebaSegmenter(std::shared_ptr<JiebaDictionary> dict,
                          JiebaMode mode = JiebaMode::kCutForSearch);

  absl::StatusOr<std::vector<std::string>> Segment(
      absl::string_view text) const override;

 private:
  std::shared_ptr<JiebaDictionary> dict_;
  JiebaMode mode_;
  // MixSegment holds only decode buffers and references the shared DictTrie.
  std::unique_ptr<cppjieba::MixSegment> mix_segment_;
};

}  // namespace valkey_search::indexes::text

#endif
```

### New file: `src/indexes/text/jieba_segmenter.cc`

```cpp
#include "src/indexes/text/jieba_segmenter.h"

#include <string>
#include <vector>

#include "cppjieba/MixSegment.hpp"
#include "cppjieba/HMMSegment.hpp"
#include "cppjieba/FullSegment.hpp"

namespace valkey_search::indexes::text {

JiebaSegmenter::JiebaSegmenter(std::shared_ptr<JiebaDictionary> dict,
                               JiebaMode mode)
    : dict_(std::move(dict)), mode_(mode) {
  // Construct MixSegment/HMMSegment using the shared DictTrie and HMMModel.
  // These are read-only references — no per-instance dictionary copy.
  // Only decode buffers (~60 KB) are per-instance.
  mix_segment_ = std::make_unique<cppjieba::MixSegment>(
      &dict_->GetDictTrie(), &dict_->GetHMMModel());
}

absl::StatusOr<std::vector<std::string>> JiebaSegmenter::Segment(
    absl::string_view text) const {
  if (text.empty()) return std::vector<std::string>{};

  std::string input(text);
  std::vector<std::string> tokens;

  switch (mode_) {
    case JiebaMode::kCutForSearch: {
      // CutForSearch: MixSegment first, then expand long tokens into
      // sub-words. Produces overlapping tokens for compound words.
      // "量子计算" → ["量子", "计算", "量子计算"]
      std::vector<cppjieba::Word> words;
      mix_segment_->CutForSearch(input, words);
      tokens.reserve(words.size());
      for (auto& w : words) {
        tokens.push_back(std::move(w.word));
      }
      break;
    }
    case JiebaMode::kQuerySegment: {
      // QuerySegment: Same CutForSearch decomposition but intended for
      // query-time use. Future enhancement: apply keyword-extraction
      // weighting (TF-IDF) to prefer high-IDF compound terms.
      std::vector<cppjieba::Word> words;
      mix_segment_->CutForSearch(input, words);
      tokens.reserve(words.size());
      for (auto& w : words) {
        tokens.push_back(std::move(w.word));
      }
      break;
    }
  }

  return tokens;
}

}  // namespace valkey_search::indexes::text
```

**Key design decisions**:
- **Primary approach (preferred)**: Use cppjieba's lower-level `MixSegment` and `HMMSegment` classes directly, passing them pointers to the shared `DictTrie` and `HMMModel` from `JiebaDictionary`. These classes accept external references and do NOT reload the dictionary. Each `JiebaSegmenter` instance then holds only lightweight decode buffers (~60 KB) — no `thread_local` needed, no per-thread dict copy. The DictTrie is read-only after construction and safe to share across threads without synchronization.
- **Fallback (contingency only)**: If byronhe's fork restructures internals such that `MixSegment`/`HMMSegment` cannot cleanly accept external `DictTrie*`, fall back to `thread_local cppjieba::Jieba` with the same file paths. In this case, the OS page cache deduplicates the physical pages (verify with `/proc/self/smaps`), but virtual address space and trie node pointers would be duplicated. This is strictly a last resort.
- The `JiebaDictionary` singleton holds the shared read-only data; all segmenter instances reference it via `shared_ptr`.

---

## 5. Chinese Stop-Word List

### New file: `src/indexes/text/chinese_stop_words.h`

**Purpose**: Provides a default stop-word list for Chinese, analogous to how OpenSearch's SmartChinese analyzer bundles ~746 stop words.

```cpp
#ifndef VALKEY_SEARCH_INDEXES_TEXT_CHINESE_STOP_WORDS_H_
#define VALKEY_SEARCH_INDEXES_TEXT_CHINESE_STOP_WORDS_H_

#include <string>
#include <vector>

namespace valkey_search::indexes::text {

/// Default Chinese stop words (high-frequency function words with low
/// information content). Sourced from Lucene's SmartChinese analyzer list.
/// These are the most common grammatical particles, conjunctions, and
/// auxiliaries that add noise to BM25 scoring without contributing to
/// retrieval relevance.
///
/// Users can override via FT.CREATE ... STOPWORDS <count> <word>...
inline const std::vector<std::string>& GetDefaultChineseStopWords() {
  static const std::vector<std::string> kStopWords = {
      "的", "了", "在", "是", "我", "有", "和", "就", "不", "人",
      "都", "一", "一个", "上", "也", "很", "到", "说", "要", "去",
      "你", "会", "着", "没有", "看", "好", "自己", "这", "他", "她",
      "它", "们", "那", "些", "为", "所", "以", "但", "而", "与",
      "或", "从", "被", "把", "让", "向", "对", "于", "之", "其",
      "中", "将", "能", "可以", "这个", "那个", "什么", "怎么", "如何",
      "哪", "啊", "吗", "呢", "吧", "嘛", "呀", "哦", "哈",
  };
  return kStopWords;
}

}  // namespace valkey_search::indexes::text

#endif
```

---

## 6. Segmenter-Delegated Query Tokenizer (CJK)

### New file: `src/indexes/text/segmenter_query_tokenizer.h`

**Purpose**: For CJK languages, the query tokenizer cannot walk codepoints looking for delimiter boundaries (Chinese has no spaces). Instead, it extracts the full text span between query-syntax characters and delegates to the `Segmenter` for word boundary detection. This is the CJK counterpart to `DelimiterQueryTokenizer`.

```cpp
#ifndef VALKEY_SEARCH_INDEXES_TEXT_SEGMENTER_QUERY_TOKENIZER_H_
#define VALKEY_SEARCH_INDEXES_TEXT_SEGMENTER_QUERY_TOKENIZER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "src/indexes/text/query_tokenizer.h"
#include "src/indexes/text/segmenter.h"

namespace valkey_search::indexes::text {

/// Segmenter-delegated query tokenizer for CJK languages.
///
/// Strategy: extract the entire text span (up to the next query-syntax
/// character or quote), then segment it in one shot. Returns tokens
/// one at a time via an internal buffer that is refilled as needed.
///
/// This differs from DelimiterQueryTokenizer which walks codepoints
/// individually — CJK requires dictionary/model-based boundaries that
/// can only be computed over multi-character spans.
class SegmenterQueryTokenizer : public QueryTokenizer {
 public:
  explicit SegmenterQueryTokenizer(std::shared_ptr<Segmenter> segmenter);

  absl::StatusOr<std::optional<Token>> NextQuotedToken(
      absl::string_view text, size_t pos) const override;

  absl::StatusOr<std::optional<Token>> NextUnquotedToken(
      absl::string_view text, size_t pos, bool& hit_query_syntax,
      bool (*IsQuerySyntax)(uint32_t cp)) const override;

 private:
  std::shared_ptr<Segmenter> segmenter_;

  // Extracts a UTF-8 span from text[pos..] up to the given boundary
  // character (quote or query syntax). Returns the span and bytes consumed.
  struct Span {
    std::string text;
    size_t bytes_consumed;
  };
  Span ExtractSpan(absl::string_view text, size_t pos,
                   bool stop_on_quote,
                   bool (*IsQuerySyntax)(uint32_t cp),
                   bool& hit_query_syntax) const;
};

}  // namespace valkey_search::indexes::text

#endif
```

### New file: `src/indexes/text/segmenter_query_tokenizer.cc`

Implementation extracts the full text span, segments it, and returns the first token with `bytes_consumed` covering the entire span. The query parser will receive all tokens from a single call by iterating — the contract is that one call to `NextUnquotedToken` / `NextQuotedToken` returns one token and advances past the consumed bytes.

**Detailed behavior**: Since the segmenter operates on spans (not single codepoints), this tokenizer:
1. Scans forward from `pos` until hitting a query-syntax char (unquoted) or `"` (quoted).
2. Passes the entire extracted span to `segmenter_->Segment()`.
3. Returns the first token with `bytes_consumed = span.bytes_consumed`.
4. On subsequent calls (pos advanced past the span), the next span starts.

This means the parser sees one token per segmenter output, with the `bytes_consumed` reflecting the full span on the first token. The parser advances by `bytes_consumed` and will find itself at the next query-syntax boundary.

**Alternative**: Buffer all tokens from the span and return them one at a time with `bytes_consumed = 0` for all but the last. This preserves the "advance by bytes_consumed" contract more granularly. The implementation should use this approach — store a `mutable std::vector<std::string> pending_tokens_` and `mutable size_t pending_span_bytes_` to track buffered state.

---

## 7. Chinese Processor Factory

### New file: `src/indexes/text/chinese_processor.h`

```cpp
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
///   Segmenter:      JiebaSegmenter(CutForSearch mode, shared DictTrie)
///   QueryTokenizer: SegmenterQueryTokenizer(JiebaSegmenter in QuerySegment mode)
///   Filters:        NormalizeCaseFoldFilter(NFKC) → StopWordFilter(chinese_stops)
///   Stemmer:        nullptr (Chinese does not use stemming)
///
/// Memory model:
///   - DictTrie + HMMModel: ~60 MB shared across all Chinese processors
///   - Per-processor overhead: ~60 KB (HMM decode buffers, result vectors)
///
/// The factory loads the dictionary lazily on first call. Subsequent calls
/// reuse the same shared_ptr<JiebaDictionary>.
std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::vector<std::string>& stop_words);

}  // namespace valkey_search::indexes::text

#endif
```

### New file: `src/indexes/text/chinese_processor.cc`

```cpp
#include "src/indexes/text/chinese_processor.h"

#include "src/indexes/text/chinese_stop_words.h"
#include "src/indexes/text/jieba_dict_trie.h"
#include "src/indexes/text/jieba_segmenter.h"
#include "src/indexes/text/normalize_case_fold_filter.h"
#include "src/indexes/text/segmenter_query_tokenizer.h"
#include "src/indexes/text/stop_word_filter.h"
#include "src/indexes/text/unicode_normalizer.h"

// Dictionary paths — resolved at build time via CMake CPPJIEBA_DICT_DIR.
// At runtime, these are relative to the module's load path or configured
// via a valkey-search module option.
#ifndef CPPJIEBA_DICT_DIR
#define CPPJIEBA_DICT_DIR "/usr/share/valkey-search/jieba-dict"
#endif

namespace valkey_search::indexes::text {

std::shared_ptr<LanguageProcessor> CreateChineseProcessor(
    const std::vector<std::string>& stop_words) {
  auto processor = std::make_shared<LanguageProcessor>();

  // Load or reuse the shared dictionary
  std::string dict_dir = CPPJIEBA_DICT_DIR;
  auto dictionary = JiebaDictionary::GetInstance(
      dict_dir + "/jieba.dict.utf8",
      dict_dir + "/hmm_model.utf8",
      dict_dir + "/user.dict.utf8",
      dict_dir + "/idf.utf8",
      dict_dir + "/stop_words.utf8");

  // Segmenter: CutForSearch mode for ingestion (overlapping tokens)
  auto ingestion_segmenter =
      std::make_shared<JiebaSegmenter>(dictionary, JiebaMode::kCutForSearch);
  processor->segmenters_.push_back(ingestion_segmenter);

  // Query tokenizer: uses QuerySegment mode for query-time segmentation
  auto query_segmenter =
      std::make_shared<JiebaSegmenter>(dictionary, JiebaMode::kQuerySegment);
  processor->query_tokenizer_ =
      std::make_shared<SegmenterQueryTokenizer>(query_segmenter);

  // Filter 1: NFKC normalization + case folding
  auto normalizer = std::make_shared<NormalizeCaseFoldFilter>(NormalizationForm::NFKC);
  processor->normalizer_ = normalizer;
  processor->filters_.push_back(std::move(normalizer));

  // Filter 2: Stop word removal
  // Use provided stop words if non-empty, else fall back to defaults
  const auto& effective_stops =
      stop_words.empty() ? GetDefaultChineseStopWords() : stop_words;
  auto stop_filter = std::make_shared<StopWordFilter>(effective_stops);
  processor->stop_word_filter_ = stop_filter;
  processor->filters_.push_back(std::move(stop_filter));

  // No stemmer for Chinese
  processor->stemmer_ = nullptr;

  return processor;
}

}  // namespace valkey_search::indexes::text
```

---

## 8. Plug Into the Factory

### Modified file: `src/indexes/text/language_processor.cc`

```diff
 #include "src/indexes/text/language_processor.h"

 #include <utility>

+#include "src/indexes/text/chinese_processor.h"
 #include "src/indexes/text/snowball_processor.h"

 namespace valkey_search::indexes::text {

 // ... Segment(), ApplyFilters(), Process() unchanged ...

 std::shared_ptr<LanguageProcessor> LanguageProcessor::Create(
     data_model::Language language, const std::string &punctuation,
     const std::vector<std::string> &stop_words) {
   switch (language) {
-      // TODO: Add ICU processor cases here when implemented
-      // case data_model::LANGUAGE_CHINESE:
-      // case data_model::LANGUAGE_JAPANESE:
-      // case data_model::LANGUAGE_KOREAN:
-      //   return CreateICUProcessor(language, stop_words);
+    case data_model::LANGUAGE_CHINESE:
+      return CreateChineseProcessor(stop_words);

     default:
       return CreateSnowballProcessor(language, punctuation, stop_words);
   }
 }
```

---

## 9. CMakeLists.txt Updates

### Modified file: `src/indexes/CMakeLists.txt`

Add new source files to `SRCS_TEXT`:

```cmake
set(SRCS_TEXT ${CMAKE_CURRENT_LIST_DIR}/text/text_index.h
              # ... existing files ...
              ${CMAKE_CURRENT_LIST_DIR}/text/unicode_normalizer.h
+             ${CMAKE_CURRENT_LIST_DIR}/text/jieba_dict_trie.h
+             ${CMAKE_CURRENT_LIST_DIR}/text/jieba_dict_trie.cc
+             ${CMAKE_CURRENT_LIST_DIR}/text/jieba_segmenter.h
+             ${CMAKE_CURRENT_LIST_DIR}/text/jieba_segmenter.cc
+             ${CMAKE_CURRENT_LIST_DIR}/text/chinese_stop_words.h
+             ${CMAKE_CURRENT_LIST_DIR}/text/segmenter_query_tokenizer.h
+             ${CMAKE_CURRENT_LIST_DIR}/text/segmenter_query_tokenizer.cc
+             ${CMAKE_CURRENT_LIST_DIR}/text/chinese_processor.h
+             ${CMAKE_CURRENT_LIST_DIR}/text/chinese_processor.cc
              ${CMAKE_CURRENT_LIST_DIR}/text/fuzzy.h
              # ... rest ...
```

Add link dependency:

```cmake
target_link_libraries(text PUBLIC cppjieba)
```

### Modified file: `third_party/CMakeLists.txt`

```cmake
add_subdirectory(simsimd)
add_subdirectory(hdrhistogram_c)
add_subdirectory(hnswlib)
add_subdirectory(snowball)
add_subdirectory(icu)
+add_subdirectory(cppjieba)
```

---

## 10. Dictionary Path Configuration

### Modified file: `src/valkey_search_options.h` (or equivalent options file)

Add a module-load option for the dictionary path:

```cpp
/// Path to cppjieba dictionary directory.
/// Default: compiled-in CPPJIEBA_DICT_DIR from CMake.
/// Overridable via: loadmodule libsearch.so JIEBA_DICT_PATH /path/to/dict
inline ModuleOption<std::string>& GetJiebaDictPath() {
  static ModuleOption<std::string> opt("JIEBA_DICT_PATH", CPPJIEBA_DICT_DIR);
  return opt;
}
```

The `CreateChineseProcessor` factory should read this option instead of using the hardcoded `CPPJIEBA_DICT_DIR`.

---

## 11. Feature Flag Integration

### Modified file: `src/multi_language.h`

The existing `IsLanguageSupported()` already gates non-English languages behind `kRelease14` + the multi-language feature flag. `LANGUAGE_CHINESE` automatically inherits this gate — no additional changes needed. When the feature flag is disabled or version < 1.4, `FT.CREATE ... LANGUAGE chinese` will be rejected.

---

## 12. Memory Optimization Summary

| Component | Memory | Sharing |
|-----------|--------|---------|
| DictTrie (DAT from byronhe's reduced dict) | ~50 MB | Process-wide singleton (`shared_ptr`) |
| HMMModel (transition/emission matrices) | ~10 MB | Process-wide singleton (`shared_ptr`) |
| Per-thread Jieba decode buffers | ~60 KB | Per thread (`thread_local`) |
| StopWordFilter hash set | ~2 KB | Per LanguageProcessor instance |
| **Total first-thread cost** | **~60 MB** | — |
| **Each additional thread** | **~60 KB** | — |

Compared to upstream cppjieba (126 MB due to full 11 MB dict), byronhe's fork with the reduced dictionary saves ~66 MB total.

---

## 13. File Summary

| File | Action | Purpose |
|------|--------|---------|
| `src/index_schema.proto` | Modify | Add `LANGUAGE_CHINESE = 13` |
| `third_party/cppjieba/CMakeLists.txt` | Create | FetchContent for byronhe/cppjieba |
| `third_party/CMakeLists.txt` | Modify | Add `add_subdirectory(cppjieba)` |
| `src/indexes/text/jieba_dict_trie.h` | Create | Shared dictionary singleton header |
| `src/indexes/text/jieba_dict_trie.cc` | Create | Shared dictionary singleton impl |
| `src/indexes/text/jieba_segmenter.h` | Create | Segmenter interface impl header |
| `src/indexes/text/jieba_segmenter.cc` | Create | Segmenter impl (CutForSearch/QuerySegment) |
| `src/indexes/text/chinese_stop_words.h` | Create | Default Chinese stop-word list |
| `src/indexes/text/segmenter_query_tokenizer.h` | Create | CJK query tokenizer header |
| `src/indexes/text/segmenter_query_tokenizer.cc` | Create | CJK query tokenizer impl |
| `src/indexes/text/chinese_processor.h` | Create | Factory function declaration |
| `src/indexes/text/chinese_processor.cc` | Create | Factory function composition |
| `src/indexes/text/language_processor.cc` | Modify | Wire `LANGUAGE_CHINESE` case |
| `src/indexes/CMakeLists.txt` | Modify | Add new sources + link cppjieba |
| `src/valkey_search_options.h` | Modify | Add `JIEBA_DICT_PATH` option |

---

## 14. Testing Plan

| Test | Scope | Location |
|------|-------|----------|
| `JiebaDictTrieTest` | Singleton loading, thread safety | `testing/src/jieba_dict_trie_test.cc` |
| `JiebaSegmenterTest` | CutForSearch output correctness | `testing/src/jieba_segmenter_test.cc` |
| `ChineseProcessorTest` | Full pipeline: segment → normalize → stop filter | `testing/src/chinese_processor_test.cc` |
| `SegmenterQueryTokenizerTest` | Query syntax boundary handling for CJK | `testing/src/segmenter_query_tokenizer_test.cc` |
| Integration test | `FT.CREATE ... LANGUAGE chinese` + `FT.SEARCH` | `testing/integration/chinese_search_test.py` |

---

## 15. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| byronhe fork disappears or diverges | Build breaks | Pin to a specific commit SHA in FetchContent; vendor dict files if needed |
| `MixSegment`/`HMMSegment` API doesn't accept external `DictTrie*` in fork | Can't share dict directly | Fall back to `thread_local cppjieba::Jieba` with same file paths; OS page cache deduplicates physical pages (verify via `/proc/self/smaps`). Alternatively, patch the fork to expose the pointer-accepting constructor. |
| CutForSearch produces too many tokens for short queries | BM25 noise | QuerySegment mode + stop-word filtering addresses this; measure MAP regression |
| Dictionary path not found at runtime | Segfault on FT.CREATE | Add validation in `JiebaDictionary::GetInstance()` returning `absl::Status` instead of crashing; surface as `FT.CREATE` error |

---

## 16. Sequence of Implementation

1. Proto change + regenerate
2. `third_party/cppjieba/CMakeLists.txt` + verify FetchContent pulls correctly
3. `jieba_dict_trie.h/.cc` — build and unit test singleton
4. `jieba_segmenter.h/.cc` — build and unit test segmentation output
5. `chinese_stop_words.h` — static data, no test needed
6. `segmenter_query_tokenizer.h/.cc` — build and unit test
7. `chinese_processor.h/.cc` — compose pipeline, integration test
8. `language_processor.cc` — wire factory case
9. End-to-end: `FT.CREATE idx ON HASH PREFIX 1 doc: LANGUAGE chinese SCHEMA text TEXT` → index Chinese documents → `FT.SEARCH` with Chinese queries
