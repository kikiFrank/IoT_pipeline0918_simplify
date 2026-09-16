# Install script for directory: /home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so"
         RPATH "")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins" TYPE SHARED_LIBRARY PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ FILES "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/libgstqtimlpostprocess_roi.so")
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/libgstqtimlpostprocess_roi.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so"
         RPATH "")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules" TYPE SHARED_LIBRARY PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ FILES "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/libml-postprocess-yolo-nas.so")
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo-nas.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so"
         RPATH "")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules" TYPE SHARED_LIBRARY PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ FILES "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/libml-postprocess-yolo26-otm.so")
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-otm.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so"
         RPATH "")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules" TYPE SHARED_LIBRARY PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ FILES "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/libml-postprocess-yolo26-obb.so")
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-yolo26-obb.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so"
         RPATH "")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules" TYPE SHARED_LIBRARY PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ FILES "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/libml-postprocess-ppyoloe.so")
  if(EXISTS "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/plugins/ml_modules/libml-postprocess-ppyoloe.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT)
  set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
file(WRITE "/home/ubuntu/workspace/IoT_pipeline0825/plugins/mlpostprocess_roi/build_board/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
