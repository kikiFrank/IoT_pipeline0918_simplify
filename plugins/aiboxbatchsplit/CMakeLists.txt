cmake_minimum_required(VERSION 3.16)
project(gstaiboxbatchsplit VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0>=1.16)

add_library(gstaiboxbatchsplit SHARED
    gstaiboxbatchsplit.cpp
)

target_compile_definitions(gstaiboxbatchsplit PRIVATE
    PACKAGE="gstaiboxbatchsplit"
)

target_include_directories(gstaiboxbatchsplit PRIVATE
    ${GST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gstaiboxbatchsplit
    ${GST_LIBRARIES}
)

set_target_properties(gstaiboxbatchsplit PROPERTIES
    CXX_STANDARD 17
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN"
)

install(TARGETS gstaiboxbatchsplit
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
