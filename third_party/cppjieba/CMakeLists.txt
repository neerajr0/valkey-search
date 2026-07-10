# cppjieba — Header-only Chinese segmentation (MIT license)
# Uses byronhe's fork with reduced dictionary (~5 MB on disk → ~60 MB RSS
# vs upstream's 11 MB on disk → ~126 MB RSS).
#
# License: MIT (see LICENSE file in this directory)
# Upstream: https://github.com/yanyiwu/cppjieba
# Fork:     https://github.com/byronhe/cppjieba
#
# Bundled dependencies (also MIT-licensed):
#   - limonp: https://github.com/yanyiwu/limonp (MIT)
#   - darts-clone: https://github.com/s-yata/darts-clone (BSD)

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

# cppjieba is header-only; create an INTERFACE library
add_library(cppjieba INTERFACE)
target_include_directories(cppjieba INTERFACE
  ${cppjieba_SOURCE_DIR}/include
  ${cppjieba_SOURCE_DIR}/deps
)

# limonp's Md5.cpp is needed by DatTrie.hpp for dictionary cache validation.
# Build it as a small static library and link it.
add_library(limonp_md5 STATIC
  ${cppjieba_SOURCE_DIR}/deps/limonp/Md5.cpp
)
target_include_directories(limonp_md5 PUBLIC
  ${cppjieba_SOURCE_DIR}/deps
)
# Suppress deprecation warnings from limonp's use of std::ptr_fun etc.
target_compile_options(limonp_md5 PRIVATE -Wno-deprecated-declarations)

target_link_libraries(cppjieba INTERFACE limonp_md5)

# Export the dictionary path for embedding at build time
set(CPPJIEBA_DICT_DIR "${cppjieba_SOURCE_DIR}/dict"
    CACHE PATH "Path to cppjieba dictionary files" FORCE)
