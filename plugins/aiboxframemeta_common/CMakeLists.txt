cmake_minimum_required(VERSION 3.16)
project(gstaiboxframemeta VERSION 1.0.0 LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.16)

add_library(gstaiboxframemeta SHARED
    gstaiboxframemeta.cpp
)

target_compile_definitions(gstaiboxframemeta PRIVATE
    PACKAGE="gstaiboxframemeta"
)

target_include_directories(gstaiboxframemeta PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(gstaiboxframemeta PRIVATE
    ${GST_INCLUDE_DIRS}
)

target_link_libraries(gstaiboxframemeta
    ${GST_LIBRARIES}
)

set_target_properties(gstaiboxframemeta PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN"
)

install(TARGETS gstaiboxframemeta
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
