cmake_minimum_required(VERSION 3.26)
project(ivf_rabitq_external_benches LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

get_filename_component(ZVEC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(FAISS_ROOT "${ZVEC_ROOT}/thirdparty/faiss/faiss"
    CACHE PATH "Faiss source root")

if(NOT EXISTS "${FAISS_ROOT}/faiss/index_factory.h")
  message(FATAL_ERROR
      "Faiss submodule is missing; run git submodule update --init "
      "thirdparty/faiss/faiss")
endif()
if(NOT EXISTS
    "${ZVEC_ROOT}/thirdparty/RaBitQ-Library/RaBitQ-Library-0.1/include/rabitqlib/index/ivf/ivf.hpp")
  message(FATAL_ERROR
      "RaBitQ-Library submodule is missing; run git submodule update --init "
      "thirdparty/RaBitQ-Library/RaBitQ-Library-0.1")
endif()

set(FAISS_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_EXTRAS OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_MKL OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_METAL OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_SVS OFF CACHE BOOL "" FORCE)
set(FAISS_OPT_LEVEL "avx2" CACHE STRING "Faiss SIMD target")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR
      "The pinned RaBitQ-Library fast-scan implementation requires x86-64 "
      "AVX2 or AVX-512; this three-way benchmark is not supported on "
      "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(NOT FAISS_OPT_LEVEL STREQUAL "avx2"
   AND NOT FAISS_OPT_LEVEL STREQUAL "avx512"
   AND NOT FAISS_OPT_LEVEL STREQUAL "avx512_spr")
  message(FATAL_ERROR
      "FAISS_OPT_LEVEL must be avx2, avx512, or avx512_spr so Faiss and "
      "RaBitQ-Library use the same x86 SIMD target")
endif()

find_package(OpenMP REQUIRED)
include("${ZVEC_ROOT}/cmake/utils.cmake")
add_subdirectory("${FAISS_ROOT}" "${CMAKE_BINARY_DIR}/faiss" EXCLUDE_FROM_ALL)
add_subdirectory("${ZVEC_ROOT}/thirdparty/RaBitQ-Library"
                 "${CMAKE_BINARY_DIR}/rabitq-library" EXCLUDE_FROM_ALL)
include("${FAISS_ROOT}/cmake/link_to_faiss_lib.cmake")

add_executable(ivf_rabitq_faiss_bench faiss_bench.cc)
link_to_faiss_lib(ivf_rabitq_faiss_bench)

add_executable(ivf_rabitq_library_bench rabitq_library_bench.cc)
target_link_libraries(ivf_rabitq_library_bench PRIVATE
  rabitqlib OpenMP::OpenMP_CXX)
link_to_faiss_lib(ivf_rabitq_library_bench)
