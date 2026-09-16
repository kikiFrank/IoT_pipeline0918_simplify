cmake_minimum_required(VERSION 3.16)
project(gstaiboxflowtap VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.16)

add_library(gstaiboxflowtap SHARED gstaiboxflowtap.cpp)

target_compile_definitions(gstaiboxflowtap PRIVATE PACKAGE="gstaiboxflowtap")
target_include_directories(gstaiboxflowtap PRIVATE
    ${GST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(gstaiboxflowtap ${GST_LIBRARIES})

set_target_properties(gstaiboxflowtap PROPERTIES
    CXX_STANDARD 17
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN")

install(TARGETS gstaiboxflowtap LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0)
