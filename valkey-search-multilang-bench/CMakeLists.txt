cmake_minimum_required(VERSION 3.16)

project(
  ValkeySearchMultilangBench
  VERSION 0.1.0
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(ENABLE_ICU "Build ICU tokenizer backend" ON)
option(ENABLE_CPPJIEBA "Build cppjieba tokenizer backend" ON)
option(ENABLE_FRISO "Build Friso tokenizer backend" ON)

# --- Third-party dependencies ---

# cppjieba (header-only, MIT)
if(ENABLE_CPPJIEBA)
  set(CPPJIEBA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cppjieba)
  if(NOT EXISTS ${CPPJIEBA_DIR}/include/cppjieba/Jieba.hpp)
    message(FATAL_ERROR
      "cppjieba not found. Run: git submodule update --init --recursive")
  endif()

  add_library(cppjieba INTERFACE)
  target_include_directories(cppjieba INTERFACE
    ${CPPJIEBA_DIR}/include
    ${CPPJIEBA_DIR}/deps/limonp/include
  )
  target_compile_definitions(cppjieba INTERFACE ENABLE_CPPJIEBA)
endif()

# Friso (C library, MIT)
if(ENABLE_FRISO)
  set(FRISO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/friso)
  if(NOT EXISTS ${FRISO_DIR}/src/friso.h)
    message(FATAL_ERROR
      "Friso not found. Run: git submodule update --init --recursive")
  endif()

  file(GLOB FRISO_SOURCES ${FRISO_DIR}/src/*.c)
  add_library(friso STATIC ${FRISO_SOURCES})
  target_include_directories(friso PUBLIC ${FRISO_DIR}/src)
  target_compile_definitions(friso PUBLIC ENABLE_FRISO)
  # Friso uses some POSIX features
  target_compile_options(friso PRIVATE -Wno-implicit-function-declaration)
endif()

# ICU (system or custom path)
if(ENABLE_ICU)
  find_package(ICU COMPONENTS uc i18n data)
  if(ICU_FOUND)
    message(STATUS "Found system ICU: ${ICU_VERSION}")
  else()
    # Fall back to pkg-config
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
      pkg_check_modules(ICU IMPORTED_TARGET icu-uc icu-i18n)
      if(ICU_FOUND)
        message(STATUS "Found ICU via pkg-config")
      endif()
    endif()
  endif()

  if(NOT ICU_FOUND)
    message(WARNING "ICU not found. ICU tokenizer will be disabled. "
      "Install libicu-dev or set ICU_ROOT.")
    set(ENABLE_ICU OFF)
  endif()
endif()

# nlohmann/json for output (header-only, fetch if not present)
include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# --- Layer 1: Tokenizer Quality Harness ---
add_subdirectory(layer1)
