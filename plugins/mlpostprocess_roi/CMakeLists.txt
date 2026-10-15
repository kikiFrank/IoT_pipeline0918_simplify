cmake_minimum_required(VERSION 3.16)

# Defaults for standalone builds
if(NOT DEFINED GST_PLUGINS_QTI_OSS_VERSION)
  set(GST_PLUGINS_QTI_OSS_VERSION "1.0")
endif()
if(NOT DEFINED GST_VERSION_REQUIRED)
  set(GST_VERSION_REQUIRED "1.16")
endif()

project(GST_PLUGIN_QTI_OSS_ML_POST_PROCESS_ROI
  VERSION ${GST_PLUGINS_QTI_OSS_VERSION}
  LANGUAGES C CXX
)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

include_directories(${SYSROOT_INCDIR})
link_directories(${SYSROOT_LIBDIR})

find_package(PkgConfig)

if(DEFINED SYSROOT_LIBDIR AND NOT "${SYSROOT_LIBDIR}" STREQUAL "")
  set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${SYSROOT_LIBDIR}/pkgconfig")
endif()

# Get the pkgconfigs exported by the automake tools
pkg_check_modules(GST
  REQUIRED gstreamer-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(GST_BASE
  REQUIRED gstreamer-base-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(GST_ALLOC
  REQUIRED gstreamer-allocators-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(GST_VIDEO
  REQUIRED gstreamer-video-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(CAIRO REQUIRED cairo)

# QTI packages - try pkg-config first, fallback to source-tree headers
pkg_check_modules(GST_QCOM_UTILS QUIET gstreamer-qcom-oss-utils-1.0>=1.0.0)
pkg_check_modules(GST_QCOM_VIDEO QUIET gstreamer-qcom-oss-video-1.0>=1.0.0)
pkg_check_modules(GST_QCOM_ALLOC QUIET gstreamer-qcom-oss-allocators-1.0>=1.0.0)
pkg_check_modules(GST_QCOM_ML QUIET gstreamer-qcom-oss-ml-1.0>=1.0.0)
pkg_check_modules(GST_QCOM_MEM QUIET gstreamer-qcom-oss-memory-1.0>=1.0.0)

set(USE_QTI_PKGCONFIG FALSE)
if(GST_QCOM_UTILS_FOUND
   AND GST_QCOM_VIDEO_FOUND
   AND GST_QCOM_ALLOC_FOUND
   AND GST_QCOM_ML_FOUND
   AND GST_QCOM_MEM_FOUND)
  set(USE_QTI_PKGCONFIG TRUE)
endif()

# QTI_SOURCE_ROOT: 可通过命令行参数 -DQTI_SOURCE_ROOT=<path> 覆盖
if(NOT DEFINED QTI_SOURCE_ROOT)
  if(EXISTS "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/gst-plugin-base/gst/ml/ml-info.h")
    set(QTI_SOURCE_ROOT "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/gst-plugin-base")
  elseif(EXISTS "/mnt/workspace/pipeline_test/gst-plugin-base/gst/ml/ml-info.h")
    set(QTI_SOURCE_ROOT "/mnt/workspace/pipeline_test/gst-plugin-base")
  endif()
endif()

set(QTI_LINK_DIRS "")

if(USE_QTI_PKGCONFIG)
  message(STATUS "Using QTI pkg-config packages for mlpostprocess_roi")
  set(QTI_INCLUDE_DIRS
    ${GST_QCOM_UTILS_INCLUDE_DIRS}
    ${GST_QCOM_VIDEO_INCLUDE_DIRS}
    ${GST_QCOM_ALLOC_INCLUDE_DIRS}
    ${GST_QCOM_ML_INCLUDE_DIRS}
    ${GST_QCOM_MEM_INCLUDE_DIRS}
  )
  set(QTI_LINK_LIBRARIES
    ${GST_QCOM_ML_LIBRARIES}
    ${GST_QCOM_VIDEO_LIBRARIES}
    ${GST_QCOM_MEM_LIBRARIES}
    ${GST_QCOM_UTILS_LIBRARIES}
    ${GST_QCOM_ALLOC_LIBRARIES}
  )
else()
  # Fallback: look for build directory (板卡常见路径)
  if(NOT DEFINED QTI_BUILD_DIR)
    if(EXISTS "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/build_tmp_multi_cam_fix/gst-plugin-base/gst/ml")
      set(QTI_BUILD_DIR "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/build_tmp_multi_cam_fix/gst-plugin-base")
    elseif(EXISTS "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/build_stage1b2_core/gst-plugin-base/gst/ml")
      set(QTI_BUILD_DIR "/mnt/workspace/gst-plugins-qti-oss-imsdk.lnx.2.0.0.r2-rel/build_stage1b2_core/gst-plugin-base")
    elseif(EXISTS "/mnt/workspace/build-plugins/gst-plugin-base/gst/ml")
      set(QTI_BUILD_DIR "/mnt/workspace/build-plugins/gst-plugin-base")
    endif()
  endif()

  if(NOT DEFINED QTI_BUILD_DIR)
    message(FATAL_ERROR
      "QTI pkg-config packages not found and QTI_BUILD_DIR is unset. "
      "Please set -DQTI_BUILD_DIR=<path-to-gst-plugin-base-build>.")
  endif()

  if(NOT EXISTS "${QTI_SOURCE_ROOT}/gst/ml/ml-info.h")
    message(FATAL_ERROR
      "Cannot find QTI source headers at ${QTI_SOURCE_ROOT}. "
      "Set QTI_SOURCE_ROOT or fix source tree layout.")
  endif()

  if(NOT EXISTS "${QTI_BUILD_DIR}/gst/ml"
     OR NOT EXISTS "${QTI_BUILD_DIR}/gst/utils"
     OR NOT EXISTS "${QTI_BUILD_DIR}/gst/video"
     OR NOT EXISTS "${QTI_BUILD_DIR}/gst/allocators"
     OR NOT EXISTS "${QTI_BUILD_DIR}/gst/memory")
    message(FATAL_ERROR
      "QTI_BUILD_DIR does not look like a gst-plugin-base build tree: ${QTI_BUILD_DIR}")
  endif()

  message(STATUS "QTI pkg-config packages not found, using fallback mode")
  message(STATUS "Using QTI source headers from: ${QTI_SOURCE_ROOT}")
  message(STATUS "Using QTI build libs from: ${QTI_BUILD_DIR}")

  set(QTI_INCLUDE_DIRS ${QTI_SOURCE_ROOT})
  set(QTI_LINK_DIRS
    ${QTI_BUILD_DIR}/gst/ml
    ${QTI_BUILD_DIR}/gst/utils
    ${QTI_BUILD_DIR}/gst/video
    ${QTI_BUILD_DIR}/gst/allocators
    ${QTI_BUILD_DIR}/gst/memory
  )
  set(QTI_LINK_LIBRARIES
    /usr/lib/aarch64-linux-gnu/libgstqtimlbase.so.1
    /usr/lib/aarch64-linux-gnu/libgstqtivideobase.so.1
    /usr/lib/aarch64-linux-gnu/libgstqtimemorybase.so.1
    /usr/lib/aarch64-linux-gnu/libgstqtiutilsbase.so.1
    /usr/lib/aarch64-linux-gnu/libgstqtiallocatorsbase.so.1
  )
endif()

# Generate configuration header file
configure_file(config.h.in config.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Check whether DMA header is present
include(CheckIncludeFile)
check_include_file("linux/dma-buf.h" HAVE_LINUX_DMA_BUF_H)

# Precompiler definitions
add_definitions(-DHAVE_CONFIG_H)

# Common compiler flags (no -Wextra/-Werror for forked code with pre-existing warnings)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wno-missing-field-initializers")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-missing-field-initializers")

# GStreamer ML post process plugin with ROI meta
set(GST_QTI_ML_POST_PROCESS_ROI gstqtimlpostprocess_roi)

add_library(${GST_QTI_ML_POST_PROCESS_ROI} SHARED
  mlpostprocess.cc
  mlpostprocess-utils.cc
)

# GST_ML_MODULES_DIR: use project-local directory to avoid polluting system.
# At runtime the plugin will dlopen("<dir>/libml-postprocess-<nick>.so").
# Falls back to system path only if the local directory does not exist at cmake time.
# Module libraries are built into this directory; the build script mirrors it
# to <project root>/plugins/ml_modules, which is what the runtime fallback
# below targets. Used only for build/install rules, never embedded.
set(GST_ML_MODULES_DIR_LOCAL "${CMAKE_BINARY_DIR}/plugins/ml_modules")
message(STATUS "Configured ml-postprocess modules dir (build): ${GST_ML_MODULES_DIR_LOCAL}")

file(MAKE_DIRECTORY ${GST_ML_MODULES_DIR_LOCAL})

# The module directory is resolved at runtime by gst_ml_get_modules_dir()
# (mlpostprocess-utils.cc), so a renamed or relocated project tree still finds
# its modules. No absolute path is baked in: even the compile-time fallback is
# relative to the working directory, which the start scripts set to the
# project root.
target_compile_definitions(${GST_QTI_ML_POST_PROCESS_ROI} PRIVATE
  GST_ML_MODULES_DIR_BUILD_FALLBACK="plugins/ml_modules"
  $<$<BOOL:${HAVE_LINUX_DMA_BUF_H}>:HAVE_LINUX_DMA_BUF_H>
)

target_include_directories(${GST_QTI_ML_POST_PROCESS_ROI} PUBLIC
  ${GST_INCLUDE_DIRS}
  ${QTI_INCLUDE_DIRS}
)

target_link_libraries(${GST_QTI_ML_POST_PROCESS_ROI} PRIVATE
  ${GST_LIBRARIES}
  ${GST_BASE_LIBRARIES}
  ${GST_ALLOC_LIBRARIES}
  ${GST_VIDEO_LIBRARIES}
  ${CAIRO_LIBRARIES}
  ${QTI_LINK_LIBRARIES}
)

# Install to build/plugins/ directory (not system)
install(
  TARGETS ${GST_QTI_ML_POST_PROCESS_ROI}
  LIBRARY DESTINATION ${CMAKE_BINARY_DIR}/plugins
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

# -------------------------------------------------------------------
# YOLO26 post-processing module (borrowed nick: yolo-nas)
# Produces: libml-postprocess-yolo-nas.so  →  <GST_ML_MODULES_DIR>/
# -------------------------------------------------------------------
set(YOLO26_MODULE ml-postprocess-yolo-nas)

add_library(${YOLO26_MODULE} SHARED
  modules/object-detection/ml-postprocess-yolo26.cc
)

target_include_directories(${YOLO26_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

set_target_properties(${YOLO26_MODULE} PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "${YOLO26_MODULE}"
)

install(
  TARGETS ${YOLO26_MODULE}
  LIBRARY DESTINATION ${GST_ML_MODULES_DIR_LOCAL}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

# -------------------------------------------------------------------
# YOLO26 OTM (one-to-many) post-processing module
# Produces: libml-postprocess-yolo26-otm.so  →  <GST_ML_MODULES_DIR>/
# -------------------------------------------------------------------
set(YOLO26_OTM_MODULE ml-postprocess-yolo26-otm)

add_library(${YOLO26_OTM_MODULE} SHARED
  modules/object-detection/ml-postprocess-yolo26-otm.cc
)

target_include_directories(${YOLO26_OTM_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

set_target_properties(${YOLO26_OTM_MODULE} PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "${YOLO26_OTM_MODULE}"
)

install(
  TARGETS ${YOLO26_OTM_MODULE}
  LIBRARY DESTINATION ${GST_ML_MODULES_DIR_LOCAL}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

# -------------------------------------------------------------------
# YOLO26 No-E2E (raw head, single [1,14,8400] tensor) post-processing module
# Produces: libml-postprocess-yolo26-noe2e.so  →  <GST_ML_MODULES_DIR>/
# -------------------------------------------------------------------
set(YOLO26_NOE2E_MODULE ml-postprocess-yolo26-noe2e)

add_library(${YOLO26_NOE2E_MODULE} SHARED
  modules/object-detection/ml-postprocess-yolo26-noe2e.cc
)

target_include_directories(${YOLO26_NOE2E_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

set_target_properties(${YOLO26_NOE2E_MODULE} PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "${YOLO26_NOE2E_MODULE}"
)

install(
  TARGETS ${YOLO26_NOE2E_MODULE}
  LIBRARY DESTINATION ${GST_ML_MODULES_DIR_LOCAL}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

# -------------------------------------------------------------------
# YOLO26 OBB (oriented bbox -> AABB) post-processing module
# Produces: libml-postprocess-yolo26-obb.so  →  <GST_ML_MODULES_DIR>/
# -------------------------------------------------------------------
set(YOLO26_OBB_MODULE ml-postprocess-yolo26-obb)

add_library(${YOLO26_OBB_MODULE} SHARED
  modules/object-detection/ml-postprocess-yolo26-obb.cc
)

target_include_directories(${YOLO26_OBB_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

set_target_properties(${YOLO26_OBB_MODULE} PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "${YOLO26_OBB_MODULE}"
)

install(
  TARGETS ${YOLO26_OBB_MODULE}
  LIBRARY DESTINATION ${GST_ML_MODULES_DIR_LOCAL}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

# -------------------------------------------------------------------
# PP-YOLOE helmet post-processing module
# Produces: libml-postprocess-ppyoloe.so  →  <GST_ML_MODULES_DIR>/
# -------------------------------------------------------------------
set(PPYOLOE_MODULE ml-postprocess-ppyoloe)

add_library(${PPYOLOE_MODULE} SHARED
  modules/object-detection/ml-postprocess-ppyoloe.cc
)

target_include_directories(${PPYOLOE_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/modules
)

set_target_properties(${PPYOLOE_MODULE} PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "${PPYOLOE_MODULE}"
)

install(
  TARGETS ${PPYOLOE_MODULE}
  LIBRARY DESTINATION ${GST_ML_MODULES_DIR_LOCAL}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
