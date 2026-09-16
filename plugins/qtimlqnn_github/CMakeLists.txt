cmake_minimum_required(VERSION 3.16)

project(qtimlqnn_github VERSION 0.0.0 LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.24)
pkg_check_modules(GST_BASE REQUIRED IMPORTED_TARGET gstreamer-base-1.0>=1.24)
pkg_check_modules(GST_ALLOC REQUIRED IMPORTED_TARGET gstreamer-allocators-1.0>=1.24)

set(QIM_BASE_SOURCE "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel"
    CACHE PATH "Board path containing gst-plugin-base source headers")
set(QIM_ML_BASE_LIBRARY "/usr/lib/aarch64-linux-gnu/libgstqtimlbase.so.1"
    CACHE FILEPATH "Board QIM ML base runtime library")
set(QIM_UTILS_BASE_LIBRARY "/usr/lib/aarch64-linux-gnu/libgstqtiutilsbase.so.1"
    CACHE FILEPATH "Board QIM utils base runtime library")

if(NOT EXISTS "${QIM_BASE_SOURCE}/gst-plugin-base/gst/ml/ml-info.h")
  message(FATAL_ERROR "QIM_BASE_SOURCE does not contain gst-plugin-base headers: ${QIM_BASE_SOURCE}")
endif()

foreach(qim_library IN ITEMS QIM_ML_BASE_LIBRARY QIM_UTILS_BASE_LIBRARY)
  if(NOT EXISTS "${${qim_library}}")
    message(FATAL_ERROR "Required board runtime library is missing: ${${qim_library}}")
  endif()
endforeach()

configure_file(
  source/gstreamer/gst-plugin-mlqnn/config.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/config.h"
  @ONLY)

add_library(gstqtimlqnn_github SHARED
  source/gstreamer/gst-plugin-mlqnn/mlqnn.c
  source/gstreamer/gst-plugin-mlqnn/ml-qnn-engine.cc)

set_target_properties(gstqtimlqnn_github PROPERTIES
  OUTPUT_NAME gstqtimlqnn_github
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS YES)

target_compile_definitions(gstqtimlqnn_github PRIVATE HAVE_CONFIG_H)
target_compile_options(gstqtimlqnn_github PRIVATE
  -Wall
  -Wextra
  -Werror
  -Wno-unused-parameter)
target_include_directories(gstqtimlqnn_github PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${QIM_BASE_SOURCE}/gst-plugin-base"
  "${GST_INCLUDE_DIRS}"
  "${GST_BASE_INCLUDE_DIRS}"
  "${GST_ALLOC_INCLUDE_DIRS}")
target_link_libraries(gstqtimlqnn_github PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_ALLOC
  "${QIM_ML_BASE_LIBRARY}"
  "${QIM_UTILS_BASE_LIBRARY}")
