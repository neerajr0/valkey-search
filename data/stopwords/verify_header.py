#!/usr/bin/env python3
"""
Verify that stop_words.h matches the source .txt files from Lucene.

Checks:
1. Each word in the .txt file appears in the .h file's array
2. Each word in the .h file's array appears in the .txt file
3. Word counts match
"""

import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
HEADER_PATH = os.path.join(SCRIPT_DIR, "../../src/indexes/text/stop_words.h")

LANGUAGES = {
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

# English is hardcoded (not from Lucene), skip it
ENGLISH_WORDS = [
    "a", "is", "the", "an", "and", "are", "as", "at", "be",
    "but", "by", "for", "if", "in", "into", "it", "no", "not",
    "of", "on", "or", "such", "that", "their", "then", "there", "these",
    "they", "this", "to", "was", "will", "with",
]


def parse_txt_file(filepath, comment_char):
    """Parse a stop word .txt file, returning a list of words."""
    words = []
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if comment_char == "|":
                if line.startswith("|"):
                    continue
                if "|" in line:
                    line = line[: line.index("|")]
                for word in line.split():
                    if word:
                        words.append(word)
            else:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                words.append(line)
    return words


def parse_header_array(header_content, lang):
    """Extract words from a specific language array in the header."""
    # Find the array for this language
    pattern = rf'k{lang.capitalize()}StopWords\{{\s*(.*?)\s*\}};'
    match = re.search(pattern, header_content, re.DOTALL)
    if not match:
        return None
    
    array_content = match.group(1)
    # Extract all quoted strings
    words = re.findall(r'"([^"]*)"', array_content)
    return words


def main():
    if not os.path.exists(HEADER_PATH):
        print(f"ERROR: Header not found: {HEADER_PATH}")
        sys.exit(1)

    with open(HEADER_PATH, "r", encoding="utf-8") as f:
        header_content = f.read()

    all_ok = True

    # Check English (hardcoded)
    header_english = parse_header_array(header_content, "english")
    if header_english is None:
        print("ERROR: Could not find kEnglishStopWords in header")
        all_ok = False
    elif header_english != ENGLISH_WORDS:
        print(f"MISMATCH: English")
        print(f"  Expected {len(ENGLISH_WORDS)} words, got {len(header_english)}")
        missing = set(ENGLISH_WORDS) - set(header_english)
        extra = set(header_english) - set(ENGLISH_WORDS)
        if missing:
            print(f"  Missing from header: {missing}")
        if extra:
            print(f"  Extra in header: {extra}")
        all_ok = False
    else:
        print(f"  english: OK ({len(header_english)} words)")

    # Check each language
    for lang, (filename, comment_char) in LANGUAGES.items():
        filepath = os.path.join(SCRIPT_DIR, filename)
        if not os.path.exists(filepath):
            print(f"ERROR: Missing source file: {filepath}")
            all_ok = False
            continue

        txt_words = parse_txt_file(filepath, comment_char)
        header_words = parse_header_array(header_content, lang)

        if header_words is None:
            print(f"ERROR: Could not find k{lang.capitalize()}StopWords in header")
            all_ok = False
            continue

        if txt_words == header_words:
            print(f"  {lang}: OK ({len(header_words)} words)")
        else:
            print(f"MISMATCH: {lang}")
            print(f"  .txt file: {len(txt_words)} words")
            print(f"  header:    {len(header_words)} words")
            
            txt_set = set(txt_words)
            header_set = set(header_words)
            missing = txt_set - header_set
            extra = header_set - txt_set
            if missing:
                print(f"  In .txt but NOT in header: {sorted(missing)[:10]}")
            if extra:
                print(f"  In header but NOT in .txt: {sorted(extra)[:10]}")
            
            # Check order
            if set(txt_words) == set(header_words) and txt_words != header_words:
                print(f"  (Same words but different order)")
            
            all_ok = False

    print()
    if all_ok:
        print("ALL CHECKS PASSED")
    else:
        print("SOME CHECKS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
