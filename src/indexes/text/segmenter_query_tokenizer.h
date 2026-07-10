/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

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
/// Strategy: For each call, extract the full text span up to the next
/// query-syntax character (unquoted) or closing quote (quoted), segment
/// the entire span in one shot, and return the first token. The
/// bytes_consumed covers the entire span so the parser advances past all
/// the text that was segmented. The remaining tokens from the span are
/// buffered and returned on subsequent calls (with bytes_consumed = 0).
///
/// This differs from DelimiterQueryTokenizer which walks codepoints
/// individually — CJK requires dictionary/model-based boundaries that
/// can only be computed over multi-character spans.
///
/// Thread-safety: This class uses mutable state for the token buffer.
/// Query parsing (FilterParser::Parse, which calls this tokenizer) runs on
/// the main Valkey event-loop thread during command handling — before the
/// search is dispatched to the reader thread pool. Valkey processes commands
/// sequentially on the main thread, so concurrent tokenizer access cannot
/// occur. The thread pool only executes index traversal and scoring, not
/// query parsing.
///
/// If query parsing is ever moved off the main thread, this class must be
/// made stateless (e.g., returning all tokens from a span in a single call
/// via an output vector) or the caller must instantiate per-parse copies.
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

  // Buffered tokens from the last segmentation call.
  // Mutable because the tokenizer interface is const but we need to
  // maintain state across calls within the same span.
  mutable std::vector<std::string> pending_tokens_;
  mutable size_t pending_span_bytes_ = 0;

  /// Drain the next token from the pending buffer.
  /// Returns nullopt if the buffer is empty.
  std::optional<Token> DrainPending() const;

  /// Extract a UTF-8 text span from text[pos..] up to a boundary.
  /// For quoted mode: stops at '"'.
  /// For unquoted mode: stops at any query-syntax character.
  /// Returns the extracted span string and total bytes consumed.
  struct SpanResult {
    std::string span_text;
    size_t bytes_consumed;
    bool hit_query_syntax;
  };

  SpanResult ExtractQuotedSpan(absl::string_view text, size_t pos) const;

  SpanResult ExtractUnquotedSpan(absl::string_view text, size_t pos,
                                 bool (*IsQuerySyntax)(uint32_t cp)) const;
};

}  // namespace valkey_search::indexes::text

#endif  // VALKEY_SEARCH_INDEXES_TEXT_SEGMENTER_QUERY_TOKENIZER_H_
