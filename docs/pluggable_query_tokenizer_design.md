# Pluggable Query Tokenizer Design

## Problem

The query parser currently tokenizes by walking codepoints one at a time and breaking on `Segmenter::IsDelimiter()`. This works for delimiter-based languages (European/Snowball) but breaks down for CJK, where word boundaries require dictionary/rule-based segmentation over a span of text — they can't be detected one codepoint at a time.

The parser needs a pluggable tokenization strategy so that:
- European languages continue using the efficient per-codepoint delimiter check
- CJK languages delegate to `Segment()` on raw text spans
- No language-specific branches exist in the parser itself

## Design: `QueryTokenizer` Strategy Interface

### Interface

```cpp
// src/indexes/text/query_tokenizer.h

#ifndef VALKEY_SEARCH_INDEXES_TEXT_QUERY_TOKENIZER_H_
#define VALKEY_SEARCH_INDEXES_TEXT_QUERY_TOKENIZER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "absl/strings/string_view.h"

namespace valkey_search::indexes::text {

/// Abstract interface for query-path tokenization strategy.
///
/// The parser uses this to extract tokens from raw text spans between
/// query syntax characters. Two strategies exist:
///   - Delimiter-based (European): walk codepoints, break on IsDelimiter()
///   - Segmenter-delegated (CJK): extract span, delegate to Segment()
///
/// Standalone, testable, stateless — consistent with Segmenter/TokenFilter.
class QueryTokenizer {
 public:
  virtual ~QueryTokenizer() = default;

  struct Token {
    std::string content;
    size_t bytes_consumed;  // how far to advance pos
  };

  /// Extract the next token from query text starting at `text[pos]`.
  /// Returns nullopt when no more tokens can be extracted (hit end or
  /// query-syntax boundary). The implementation decides what constitutes
  /// a word boundary.
  virtual std::optional<Token> NextToken(absl::string_view text,
                                         size_t pos) const = 0;

  /// Whether a backslash-escaped character should be kept in the current
  /// token. Delimiter-based tokenizers answer "yes if it's a delimiter";
  /// segmenter-delegated tokenizers might always answer "yes".
  virtual bool ShouldKeepEscaped(uint32_t cp) const = 0;
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_QUERY_TOKENIZER_H_
```

### Concrete Implementations

#### `DelimiterQueryTokenizer` (current behavior, European languages)

Walks codepoints one at a time, breaks on `IsDelimiter()`. This is an extraction refactor of what `ParseUnquotedTextToken` and `ParseQuotedTextToken` do today.

```cpp
class DelimiterQueryTokenizer : public QueryTokenizer {
 public:
  explicit DelimiterQueryTokenizer(const Segmenter& segmenter);

  std::optional<Token> NextToken(absl::string_view text,
                                 size_t pos) const override;

  bool ShouldKeepEscaped(uint32_t cp) const override {
    return segmenter_.IsDelimiter(cp);
  }

 private:
  const Segmenter& segmenter_;
};
```

#### `SegmenterDelegatedQueryTokenizer` (future CJK)

Extracts the raw text span up to the next query-syntax character, delegates to `Segment()`, and yields tokens one at a time from the result.

```cpp
class SegmenterDelegatedQueryTokenizer : public QueryTokenizer {
 public:
  explicit SegmenterDelegatedQueryTokenizer(const Segmenter& segmenter);

  std::optional<Token> NextToken(absl::string_view text,
                                 size_t pos) const override;

  bool ShouldKeepEscaped(uint32_t cp) const override { return true; }

 private:
  const Segmenter& segmenter_;
};
```

## Registry-Based Factory (No Conditional Branches)

Rather than an `if/else` on language family in `LanguageProcessor::Create()`, use a registry that maps languages to tokenizer factories. New languages extend the system by registering — no existing code is modified.

### Registry Interface

```cpp
// src/indexes/text/query_tokenizer_registry.h

#ifndef VALKEY_SEARCH_INDEXES_TEXT_QUERY_TOKENIZER_REGISTRY_H_
#define VALKEY_SEARCH_INDEXES_TEXT_QUERY_TOKENIZER_REGISTRY_H_

#include <functional>
#include <memory>

#include "absl/container/flat_hash_map.h"
#include "src/index_schema.pb.h"
#include "src/indexes/text/query_tokenizer.h"
#include "src/indexes/text/segmenter.h"

namespace valkey_search::indexes::text {

using QueryTokenizerFactory =
    std::function<std::shared_ptr<QueryTokenizer>(const Segmenter&)>;

/// Registry mapping languages to their query tokenizer factory.
/// Languages not explicitly registered fall back to the default
/// (delimiter-based) tokenizer.
class QueryTokenizerRegistry {
 public:
  static QueryTokenizerRegistry& Instance();

  void Register(data_model::Language language, QueryTokenizerFactory factory);

  std::shared_ptr<QueryTokenizer> Create(data_model::Language language,
                                         const Segmenter& segmenter) const;

 private:
  QueryTokenizerRegistry();
  absl::flat_hash_map<data_model::Language, QueryTokenizerFactory> factories_;
  QueryTokenizerFactory default_factory_;
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_QUERY_TOKENIZER_REGISTRY_H_
```

### Registry Implementation

```cpp
// src/indexes/text/query_tokenizer_registry.cc

#include "src/indexes/text/query_tokenizer_registry.h"

#include "src/indexes/text/delimiter_query_tokenizer.h"

namespace valkey_search::indexes::text {

QueryTokenizerRegistry& QueryTokenizerRegistry::Instance() {
  static QueryTokenizerRegistry instance;
  return instance;
}

QueryTokenizerRegistry::QueryTokenizerRegistry()
    : default_factory_([](const Segmenter& seg) {
        return std::make_shared<DelimiterQueryTokenizer>(seg);
      }) {}

void QueryTokenizerRegistry::Register(data_model::Language language,
                                       QueryTokenizerFactory factory) {
  factories_[language] = std::move(factory);
}

std::shared_ptr<QueryTokenizer> QueryTokenizerRegistry::Create(
    data_model::Language language, const Segmenter& segmenter) const {
  auto it = factories_.find(language);
  if (it != factories_.end()) {
    return it->second(segmenter);
  }
  return default_factory_(segmenter);
}

}  // namespace valkey_search::indexes::text
```

### Language Registration (Per-Module)

Each language family registers its tokenizer in its own translation unit. CJK support lives entirely in its own module:

```cpp
// src/indexes/text/cjk_registration.cc

#include "src/indexes/text/query_tokenizer_registry.h"
#include "src/indexes/text/segmenter_delegated_query_tokenizer.h"

namespace valkey_search::indexes::text {
namespace {

const bool kRegistered = [] {
  auto& registry = QueryTokenizerRegistry::Instance();
  registry.Register(data_model::CHINESE, [](const Segmenter& seg) {
    return std::make_shared<SegmenterDelegatedQueryTokenizer>(seg);
  });
  registry.Register(data_model::JAPANESE, [](const Segmenter& seg) {
    return std::make_shared<SegmenterDelegatedQueryTokenizer>(seg);
  });
  registry.Register(data_model::KOREAN, [](const Segmenter& seg) {
    return std::make_shared<SegmenterDelegatedQueryTokenizer>(seg);
  });
  return true;
}();

}  // namespace
}  // namespace valkey_search::indexes::text
```

### Usage in LanguageProcessor::Create()

A single lookup with no branching:

```cpp
processor->query_tokenizer_ =
    QueryTokenizerRegistry::Instance().Create(language, *processor->segmenters_[0]);
```

## Integration into LanguageProcessor

```cpp
class LanguageProcessor {
 public:
  // ...existing interface...

  /// Get the query tokenizer strategy. Used by the filter parser for
  /// extracting tokens from query text. O(1) access.
  const QueryTokenizer* GetQueryTokenizer() const {
    return query_tokenizer_.get();
  }

 private:
  std::shared_ptr<QueryTokenizer> query_tokenizer_;
  // ...rest unchanged...
};
```

## Parser Integration

The filter parser replaces its inline `IsDelimiter()` loop with calls to the `QueryTokenizer`:

```cpp
// Before (in ParseUnquotedTextToken):
if (segmenter->IsDelimiter(pk.cp)) break;

// After:
const auto* tokenizer = processor.GetQueryTokenizer();
auto token = tokenizer->NextToken(expression_, pos_);
```

For backslash escape logic, `HandleBackslashEscape` calls:
```cpp
tokenizer->ShouldKeepEscaped(cp)
```
instead of:
```cpp
segmenter->IsDelimiter(cp)
```

## Design Principles Alignment

| Principle | How This Satisfies It |
|-----------|----------------------|
| **Modularity** | `QueryTokenizer` is a standalone, testable interface alongside `Segmenter` and `TokenFilter` |
| **Composability** | Adding CJK = compose `ICUSegmenter` + `SegmenterDelegatedQueryTokenizer` — no parser changes |
| **Statelessness** | `NextToken()` is pure — no side effects beyond reporting bytes consumed |
| **Language agnosticism** | Parser has zero language-specific branches; all decisions live in the tokenizer implementation |
| **Open/Closed** | New languages register into the map; no modification of existing factory logic |
| **Consistent access pattern** | Exposed via `GetQueryTokenizer()` like `GetNormalizer()`, `GetSegmenter()`, `GetStemmer()` |
| **Backward-compatible** | `DelimiterQueryTokenizer` is the default; zero semantic change for Snowball languages |

## Alternative: Compile-Time Map

If the singleton/static-init pattern is undesirable, a constexpr-style enum map works for small, known strategy sets:

```cpp
enum class QueryTokenizerType { kDelimiterBased, kSegmenterDelegated };

inline const absl::flat_hash_map<data_model::Language, QueryTokenizerType>
    kQueryTokenizerStrategy = {
        {data_model::CHINESE, QueryTokenizerType::kSegmenterDelegated},
        {data_model::JAPANESE, QueryTokenizerType::kSegmenterDelegated},
        {data_model::KOREAN, QueryTokenizerType::kSegmenterDelegated},
};
```

However, the registry approach is preferred — it allows new language modules to self-register without touching a central lookup table, which scales better as the language set grows.

## Why Not Just Call Segment() for All Languages?

Forcing Snowball languages through `Segment()` in the query path would:
- Be redundant — the parser already did the splitting character by character
- Change escape handling semantics
- Add overhead for the common case (European languages are the majority of current usage)

Keeping two strategies behind a polymorphic interface isolates the cost of CJK's more complex tokenization from the simpler delimiter-based path.
