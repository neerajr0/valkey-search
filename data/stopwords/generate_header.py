#!/usr/bin/env python3
"""
Generate stop_words.h from Apache Lucene stop word text files.

Usage: python3 generate_header.py > ../../src/indexes/text/stop_words.h
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Map of language name -> (filename, comment_char)
# comment_char: '|' for snowball format, '#' for language-specific format
LANGUAGES = {
    "english": None,  # Use existing RediSearch-compatible list (see below)
    "french": ("french_stop.txt", "|"),
    "german": ("german_stop.txt", "|"),
    "spanish": ("spanish_stop.txt", "|"),
    "italian": ("italian_stop.txt", "|"),
    "portuguese": ("portuguese_stop.txt", "|"),
    "russian": ("russian_stop.txt", "|"),
    "swedish": ("swedish_stop.txt", "|"),
    "dutch": ("dutch_stop.txt", "|"),
    "indonesian": ("indonesian_stop.txt", "|"),
    "arabic": ("arabic_stop.txt", "#"),
    "turkish": ("turkish_stop.txt", "#"),
}

# The English stop word list matches RediSearch for backward compatibility.
# This is intentionally smaller than the Lucene English list.
ENGLISH_STOP_WORDS = [
    "a", "is", "the", "an", "and", "are", "as", "at", "be",
    "but", "by", "for", "if", "in", "into", "it", "no", "not",
    "of", "on", "or", "such", "that", "their", "then", "there", "these",
    "they", "this", "to", "was", "will", "with",
]


def parse_stop_words(filepath, comment_char):
    """Parse a stop word file, returning a list of words."""
    words = []
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if comment_char == "|":
                # Snowball format: line starting with | is a comment
                # Words are at the start of the line, may have | comment after
                if line.startswith("|"):
                    continue
                # Strip inline comment
                if "|" in line:
                    line = line[: line.index("|")]
                # Some files (e.g., Indonesian) have multiple space-separated
                # words on a single line representing separate stop words
                for word in line.split():
                    if word:
                        words.append(word)
            else:
                # Hash comment format
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                words.append(line)
    return words


def escape_cpp_string(s):
    """Escape a string for use in a C++ string literal."""
    # For UTF-8 strings, we just need to escape backslashes and quotes
    return s.replace("\\", "\\\\").replace('"', '\\"')


def format_word_list(words, indent="    "):
    """Format a list of words as C++ initializer list entries."""
    lines = []
    current_line = indent
    for i, word in enumerate(words):
        entry = f'"{escape_cpp_string(word)}"'
        if i < len(words) - 1:
            entry += ","
        if len(current_line) + len(entry) + 1 > 80:
            lines.append(current_line)
            current_line = indent + entry
        else:
            if current_line == indent:
                current_line += entry
            else:
                current_line += " " + entry
    if current_line.strip():
        lines.append(current_line)
    return "\n".join(lines)


def main():
    # Parse all stop word files
    all_words = {}
    for lang, source in LANGUAGES.items():
        if source is None:
            # Use hardcoded list (English)
            all_words[lang] = ENGLISH_STOP_WORDS
            print(f"  {lang}: {len(ENGLISH_STOP_WORDS)} words (RediSearch-compatible)", file=sys.stderr)
        else:
            filename, comment_char = source
            filepath = os.path.join(SCRIPT_DIR, filename)
            if not os.path.exists(filepath):
                print(f"ERROR: Missing file: {filepath}", file=sys.stderr)
                sys.exit(1)
            words = parse_stop_words(filepath, comment_char)
            all_words[lang] = words
            print(f"  {lang}: {len(words)} words", file=sys.stderr)

    # Generate header
    print(HEADER_PREFIX)
    for lang, words in all_words.items():
        const_name = f"k{lang.capitalize()}StopWords"
        print(f"// {lang.capitalize()} stop words ({len(words)} words)")
        print(f"inline const std::vector<std::string> {const_name}{{")
        print(format_word_list(words))
        print("};")
        print()

    print(LOOKUP_FUNCTION)
    print(HEADER_SUFFIX)


HEADER_PREFIX = """\
/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

//
// Per-language default stop word lists.
//
// Stop word lists sourced from Apache Lucene.
// Licensed under the Apache License, Version 2.0.
// https://github.com/apache/lucene
// https://www.apache.org/licenses/LICENSE-2.0
//
// Snowball language stop words from:
//   lucene/analysis/common/src/resources/org/apache/lucene/analysis/snowball/
//
// Arabic stop words from:
//   lucene/analysis/common/src/resources/org/apache/lucene/analysis/ar/
//
// Turkish stop words from:
//   lucene/analysis/common/src/resources/org/apache/lucene/analysis/tr/
//

#ifndef VALKEYSEARCH_SRC_INDEXES_TEXT_STOP_WORDS_H_
#define VALKEYSEARCH_SRC_INDEXES_TEXT_STOP_WORDS_H_

#include <string>
#include <vector>

#include "src/index_schema.pb.h"

namespace valkey_search::indexes::text {
"""

LOOKUP_FUNCTION = """\
// Returns the default stop word list for the given language.
// Returns English stop words for LANGUAGE_UNSPECIFIED.
inline const std::vector<std::string>& GetDefaultStopWords(
    data_model::Language language) {
  switch (language) {
    case data_model::LANGUAGE_FRENCH:
      return kFrenchStopWords;
    case data_model::LANGUAGE_GERMAN:
      return kGermanStopWords;
    case data_model::LANGUAGE_SPANISH:
      return kSpanishStopWords;
    case data_model::LANGUAGE_ITALIAN:
      return kItalianStopWords;
    case data_model::LANGUAGE_PORTUGUESE:
      return kPortugueseStopWords;
    case data_model::LANGUAGE_RUSSIAN:
      return kRussianStopWords;
    case data_model::LANGUAGE_SWEDISH:
      return kSwedishStopWords;
    case data_model::LANGUAGE_TURKISH:
      return kTurkishStopWords;
    case data_model::LANGUAGE_DUTCH:
      return kDutchStopWords;
    case data_model::LANGUAGE_INDONESIAN:
      return kIndonesianStopWords;
    case data_model::LANGUAGE_ARABIC:
      return kArabicStopWords;
    case data_model::LANGUAGE_ENGLISH:
    case data_model::LANGUAGE_UNSPECIFIED:
    default:
      return kEnglishStopWords;
  }
}

// Note: The switch cases above require the Language enum in
// index_schema.proto to define values for all supported languages
// (Task #2: Proto Language enum + kLanguageByStr parser map).
// Until those enum values are added, this file will not compile.
// The stop word data arrays above are independent and can be used
// directly by name (e.g., kFrenchStopWords) without the lookup function."""

HEADER_SUFFIX = """\

}  // namespace valkey_search::indexes::text

#endif  // VALKEYSEARCH_SRC_INDEXES_TEXT_STOP_WORDS_H_
"""


if __name__ == "__main__":
    main()
