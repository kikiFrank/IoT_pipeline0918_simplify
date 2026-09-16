cmake_minimum_required(VERSION 3.16)
project(gstaiboxcamsplit VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST       REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_BASE  REQUIRED gstreamer-base-1.0>=1.16)
pkg_check_modules(GST_APP   REQUIRED gstreamer-app-1.0>=1.16)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0>=1.16)

include(${CMAKE_CURRENT_SOURCE_DIR}/../aiboxframemeta_common/FindAiboxFrameMetaLib.cmake)

add_library(gstaiboxcamsplit SHARED
    gstaiboxcamsplit.cpp
)

target_compile_definitions(gstaiboxcamsplit PRIVATE
    PACKAGE="gstaiboxcamsplit"
)

target_include_directories(gstaiboxcamsplit PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_BASE_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../aiboxframemeta_common
)

target_link_directories(gstaiboxcamsplit PRIVATE ${GST_AIBOX_FRAME_META_DIR})
target_link_libraries(gstaiboxcamsplit
    ${GST_LIBRARIES}
    ${GST_BASE_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    gstaiboxframemeta
)

set_target_properties(gstaiboxcamsplit PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN"
)

install(TARGETS gstaiboxcamsplit
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
