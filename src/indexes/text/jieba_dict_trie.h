/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

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
/// Only initialization is guarded by std::call_once.
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
      const std::string& dict_path, const std::string& hmm_model_path,
      const std::string& user_dict_path, const std::string& idf_path,
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

#endif  // VALKEY_SEARCH_INDEXES_TEXT_JIEBA_DICT_TRIE_H_
