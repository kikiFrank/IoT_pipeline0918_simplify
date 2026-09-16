cmake_minimum_required(VERSION 3.16)
project(gstaiboxstreamdemux VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST       REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0>=1.16)

include(${CMAKE_CURRENT_SOURCE_DIR}/../aiboxframemeta_common/FindAiboxFrameMetaLib.cmake)

add_library(gstaiboxstreamdemux SHARED
    gstaiboxstreamdemux.cpp
)

target_compile_definitions(gstaiboxstreamdemux PRIVATE
    PACKAGE="gstaiboxstreamdemux"
)

target_include_directories(gstaiboxstreamdemux PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../aiboxframemeta_common
)

target_link_directories(gstaiboxstreamdemux PRIVATE ${GST_AIBOX_FRAME_META_DIR})
target_link_libraries(gstaiboxstreamdemux
    ${GST_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    gstaiboxframemeta
)

set_target_properties(gstaiboxstreamdemux PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN"
)

install(TARGETS gstaiboxstreamdemux
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
