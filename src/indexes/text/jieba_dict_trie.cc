/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/indexes/text/jieba_dict_trie.h"

namespace valkey_search::indexes::text {

std::shared_ptr<JiebaDictionary> JiebaDictionary::instance_;
std::once_flag JiebaDictionary::init_flag_;

std::shared_ptr<JiebaDictionary> JiebaDictionary::GetInstance(
    const std::string& dict_path, const std::string& hmm_model_path,
    const std::string& user_dict_path, const std::string& idf_path,
    const std::string& stop_word_path) {
  std::call_once(init_flag_, [&]() {
    instance_ = std::shared_ptr<JiebaDictionary>(new JiebaDictionary(
        dict_path, hmm_model_path, user_dict_path, idf_path, stop_word_path));
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
