/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/text/segmenter_query_tokenizer.h"

#include <utility>

#include "src/utils/scanner.h"

namespace valkey_search::indexes::text {

SegmenterQueryTokenizer::SegmenterQueryTokenizer(
    std::shared_ptr<Segmenter> segmenter)
    : segmenter_(std::move(segmenter)) {}

std::optional<QueryTokenizer::Token> SegmenterQueryTokenizer::DrainPending()
    const {
  if (pending_tokens_.empty()) {
    return std::nullopt;
  }
  // First token gets the full span's bytes_consumed so the parser advances
  // past all text that was segmented. Subsequent tokens get 0.
  size_t consumed = pending_span_bytes_;
  pending_span_bytes_ = 0;

  std::string token_content = std::move(pending_tokens_.front());
  pending_tokens_.erase(pending_tokens_.begin());
  return Token{std::move(token_content), consumed};
}

SegmenterQueryTokenizer::SpanResult SegmenterQueryTokenizer::ExtractQuotedSpan(
    absl::string_view text, size_t pos) const {
  SpanResult result;
  result.hit_query_syntax = false;
  size_t cursor = pos;

  while (cursor < text.size()) {
    if (text[cursor] == '"') {
      break;
    }
    // Advance one UTF-8 codepoint
    utils::Scanner s(text.substr(cursor));
    auto cp = s.NextUtf8();
    if (cp == utils::Scanner::kEOF) {
      break;
    }
    cursor += s.LastUtf8ByteLen();
  }

  result.span_text = std::string(text.substr(pos, cursor - pos));
  result.bytes_consumed = cursor - pos;
  return result;
}

SegmenterQueryTokenizer::SpanResult
SegmenterQueryTokenizer::ExtractUnquotedSpan(
    absl::string_view text, size_t pos,
    bool (*IsQuerySyntax)(uint32_t cp)) const {
  SpanResult result;
  result.hit_query_syntax = false;
  size_t cursor = pos;

  while (cursor < text.size()) {
    utils::Scanner s(text.substr(cursor));
    auto cp = s.NextUtf8();
    if (cp == utils::Scanner::kEOF) {
      break;
    }
    if (cp == utils::Scanner::kInvalidCp) {
      // Skip invalid UTF-8 byte
      cursor += s.LastUtf8ByteLen();
      continue;
    }
    if (IsQuerySyntax(cp)) {
      result.hit_query_syntax = true;
      break;
    }
    cursor += s.LastUtf8ByteLen();
  }

  result.span_text = std::string(text.substr(pos, cursor - pos));
  result.bytes_consumed = cursor - pos;
  return result;
}

absl::StatusOr<std::optional<QueryTokenizer::Token>>
SegmenterQueryTokenizer::NextQuotedToken(absl::string_view text,
                                         size_t pos) const {
  // If we have buffered tokens from a previous segmentation, drain them.
  if (!pending_tokens_.empty()) {
    return DrainPending();
  }

  if (pos >= text.size()) {
    return std::nullopt;
  }

  // Check if we're at the closing quote — don't consume it.
  if (text[pos] == '"') {
    return std::nullopt;
  }

  // Extract the full span up to the closing quote.
  auto span = ExtractQuotedSpan(text, pos);
  if (span.span_text.empty()) {
    return std::nullopt;
  }

  // Segment the span.
  auto segment_result = segmenter_->Segment(span.span_text);
  if (!segment_result.ok()) {
    return segment_result.status();
  }

  // Remove empty tokens.
  std::vector<std::string> tokens;
  tokens.reserve(segment_result->size());
  for (auto& t : *segment_result) {
    if (!t.empty()) {
      tokens.push_back(std::move(t));
    }
  }

  if (tokens.empty()) {
    // Span contained no meaningful tokens — still advance past it.
    return Token{"", span.bytes_consumed};
  }

  // Buffer all tokens and set span bytes for the first drain.
  pending_tokens_ = std::move(tokens);
  pending_span_bytes_ = span.bytes_consumed;
  return DrainPending();
}

absl::StatusOr<std::optional<QueryTokenizer::Token>>
SegmenterQueryTokenizer::NextUnquotedToken(
    absl::string_view text, size_t pos, bool& hit_query_syntax,
    bool (*IsQuerySyntax)(uint32_t cp)) const {
  hit_query_syntax = false;

  // If we have buffered tokens from a previous segmentation, drain them.
  if (!pending_tokens_.empty()) {
    return DrainPending();
  }

  if (pos >= text.size()) {
    return std::nullopt;
  }

  // Check if we're already at a query syntax char.
  {
    utils::Scanner s(text.substr(pos));
    auto cp = s.NextUtf8();
    if (cp != utils::Scanner::kEOF && cp != utils::Scanner::kInvalidCp &&
        IsQuerySyntax(cp)) {
      hit_query_syntax = true;
      return std::nullopt;
    }
  }

  // Extract the full span up to the next query-syntax character.
  auto span = ExtractUnquotedSpan(text, pos, IsQuerySyntax);
  hit_query_syntax = span.hit_query_syntax;

  if (span.span_text.empty()) {
    if (span.bytes_consumed > 0) {
      return Token{"", span.bytes_consumed};
    }
    return std::nullopt;
  }

  // Segment the span.
  auto segment_result = segmenter_->Segment(span.span_text);
  if (!segment_result.ok()) {
    return segment_result.status();
  }

  // Remove empty tokens.
  std::vector<std::string> tokens;
  tokens.reserve(segment_result->size());
  for (auto& t : *segment_result) {
    if (!t.empty()) {
      tokens.push_back(std::move(t));
    }
  }

  if (tokens.empty()) {
    return Token{"", span.bytes_consumed};
  }

  // Buffer all tokens and set span bytes for the first drain.
  pending_tokens_ = std::move(tokens);
  pending_span_bytes_ = span.bytes_consumed;
  return DrainPending();
}

}  // namespace valkey_search::indexes::text
