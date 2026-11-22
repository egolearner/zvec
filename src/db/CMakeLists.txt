include(${PROJECT_ROOT_DIR}/cmake/bazel.cmake)
include(${PROJECT_ROOT_DIR}/cmake/option.cmake)

# Build the vendored UTF-8 algorithms with admission on the one growable
# Snowball runtime buffer API. Only replace the runtime utilities translation
# unit. Keep generated code, the host generator and legacy allocator ABI.
get_target_property(_fts_snowball_sources snowball SOURCES)
set(_fts_snowball_utilities ${_fts_snowball_sources})
list(FILTER _fts_snowball_utilities INCLUDE REGEX "/runtime/utilities\\.c$")
get_filename_component(_fts_snowball_runtime "${_fts_snowball_utilities}" DIRECTORY)
list(REMOVE_ITEM _fts_snowball_sources "${_fts_snowball_utilities}")
list(APPEND _fts_snowball_sources "${PROJECT_ROOT_DIR}/cmake/snowball/snowball_utilities.c")
set_property(TARGET snowball PROPERTY SOURCES "${_fts_snowball_sources}")
target_include_directories(snowball PRIVATE
  "${_fts_snowball_runtime}"
  "${CMAKE_CURRENT_SOURCE_DIR}/index/column/fts_column/tokenizer")

cc_directory(common)
cc_directory(index)
cc_directory(sqlengine)

file(GLOB_RECURSE ALL_DB_SRCS *.cc *.c *.h)

# Ensure bitpacked_simd_sse41.cc is compiled with SSE4.1 flag and
# bitpacked_simd_avx2.cc with AVX2 flag in the packed zvec target as well
# (they are also compiled separately in zvec_index).
if(NOT ANDROID AND AUTO_DETECT_ARCH)
    if(HOST_ARCH MATCHES "^(x86|x64)$")
        setup_compiler_march_for_x86(_DB_MARCH_SSE _DB_MARCH_AVX2 _DB_MARCH_AVX512 _DB_MARCH_AVX512FP16)
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/index/column/fts_column/posting/bitpacked_simd_sse41.cc
            PROPERTIES COMPILE_FLAGS "${_DB_MARCH_SSE}"
        )
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/index/column/fts_column/posting/bitpacked_simd_avx2.cc
            PROPERTIES COMPILE_FLAGS "${_DB_MARCH_AVX2}"
        )
    endif()
endif()

set(ZVEC_DB_LIBRARY_OPTIONS)
if(WIN32 AND BUILD_ZVEC_SHARED)
  list(APPEND ZVEC_DB_LIBRARY_OPTIONS
    OBJECTS
    EXPORT_DEF ZVEC_DB_BUILD_SHARED
  )
endif()

cc_library(
  NAME zvec STATIC STRICT SRCS_NO_GLOB PACKED ${ZVEC_DB_LIBRARY_OPTIONS}
  SRCS ${ALL_DB_SRCS}
  INCS . ${CMAKE_CURRENT_BINARY_DIR}
  PUBINCS ${PROJECT_ROOT_DIR}/src/include/
  LIBS
    zvec_ailego
    zvec_core
    glog
    roaring
    rocksdb
    antlr4
    FastPFOR
    cppjieba
    lz4
    snowball
    Arrow::arrow_static
    Arrow::parquet_static
    Arrow::arrow_compute
    Arrow::arrow_acero
    utf8proc
  VERSION "${PROXIMA_ZVEC_VERSION}"
)
