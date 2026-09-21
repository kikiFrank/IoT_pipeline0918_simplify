cmake_minimum_required(VERSION 3.16)
project(gstaiboxbatchgate VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0>=1.16)

add_library(gstaiboxbatchgate SHARED
    gstaiboxbatchgate.cpp
)

target_compile_definitions(gstaiboxbatchgate PRIVATE
    PACKAGE="gstaiboxbatchgate"
)

target_include_directories(gstaiboxbatchgate PRIVATE
    ${GST_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gstaiboxbatchgate
    ${GST_LIBRARIES}
)

set_target_properties(gstaiboxbatchgate PROPERTIES
    CXX_STANDARD 17
    INSTALL_RPATH "$ORIGIN"
    BUILD_RPATH "$ORIGIN"
)

add_executable(test_aiboxbatchgate
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test/batch/test_aiboxbatchgate.cpp
)
target_include_directories(test_aiboxbatchgate PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_aiboxbatchgate
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
)
set_target_properties(test_aiboxbatchgate PROPERTIES CXX_STANDARD 17)

install(TARGETS gstaiboxbatchgate
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)
