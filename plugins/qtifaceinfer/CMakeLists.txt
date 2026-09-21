cmake_minimum_required(VERSION 3.16)
project(gst_plugin_faceinfer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST       REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_BASE  REQUIRED gstreamer-base-1.0>=1.16)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0>=1.16)
pkg_check_modules(GST_ALLOC REQUIRED gstreamer-allocators-1.0>=1.16)
pkg_check_modules(GST_JSON  REQUIRED json-glib-1.0)

set(PLUGIN_TARGET gstqtifaceinfer)
set(FACE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(FACE_ASYNC_TEST_HOOKS
  "Compile test-only debug hooks (debug-infer-delay-ms / debug-infer-gate) into the plugin" OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../aiboxframemeta_common/FindAiboxFrameMetaLib.cmake)

add_library(${PLUGIN_TARGET} SHARED
  # GStreamer plugin entry
  gst_plugin/gstfacedetectionmeta.c
  gst_plugin/gstfaceresultstatemeta.c
  gst_plugin/gstfaceinfer.cpp
  gst_plugin/gstfaceinfer_lifecycle.cpp
  gst_plugin/gstfaceinfer_fastpath.cpp

  # FaceProcessor orchestrator
  FaceProcessor.cpp

  # Preprocessing (OpenCL letterbox)
  face_preprocess/FacePreprocess.cpp

  # SCRFD decode + QNN inferencer
  scrfd_infer/ScrfdDecode.cpp
  scrfd_infer/QnnInferencer.cpp

  # ArcFace (compiled in, disabled at runtime via empty arcface_model)
  arcface_infer/ArcFaceInfer.cpp
  arcface_infer/QnnInferencer.cpp

  # Memory management (OpenCL / DMABuf)
  mem_management/opencl_loader.cpp
  mem_management/dma_buffer.cpp
)

target_include_directories(${PLUGIN_TARGET} PRIVATE
  ${FACE_DIR}               # FaceProcessor.hpp, FaceTypes (via scrfd_infer/)
  ${FACE_DIR}/gst_plugin    # gstfacedetectionmeta.h, gstfaceinfer.h
  ${FACE_DIR}/../aiboxframemeta_common   # gstaiboxframemeta.h (single-ABI contract)
  ${GST_INCLUDE_DIRS}
  ${GST_BASE_INCLUDE_DIRS}
  ${GST_VIDEO_INCLUDE_DIRS}
  ${GST_ALLOC_INCLUDE_DIRS}
  ${GST_JSON_INCLUDE_DIRS}
)

target_compile_definitions(${PLUGIN_TARGET} PRIVATE
  PACKAGE="qtifaceinfer"
)

if(FACE_ASYNC_TEST_HOOKS)
  target_compile_definitions(${PLUGIN_TARGET} PRIVATE FACE_ASYNC_TEST_HOOKS=1)
endif()

target_compile_options(${PLUGIN_TARGET} PRIVATE
  -Wall -Wextra
  -Wno-unused-parameter
)

target_link_directories(${PLUGIN_TARGET} PRIVATE ${GST_AIBOX_FRAME_META_DIR})
target_link_libraries(${PLUGIN_TARGET} PRIVATE
  ${GST_LIBRARIES}
  ${GST_BASE_LIBRARIES}
  ${GST_VIDEO_LIBRARIES}
  ${GST_ALLOC_LIBRARIES}
  ${GST_JSON_LIBRARIES}
  gstaiboxframemeta
  dl
)

set_target_properties(${PLUGIN_TARGET} PROPERTIES
  INSTALL_RPATH "$ORIGIN"
  BUILD_RPATH  "$ORIGIN"
)

install(TARGETS ${PLUGIN_TARGET}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
