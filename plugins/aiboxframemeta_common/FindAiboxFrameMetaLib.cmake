# Resolve libgstaiboxframemeta.so for sibling plugins.
# Prefer AIBOX_FRAMEMETA_LIB (board_build_unified exports the built path).
set(GST_AIBOX_FRAME_META_DIR "")
if(DEFINED ENV{AIBOX_FRAMEMETA_LIB} AND EXISTS "$ENV{AIBOX_FRAMEMETA_LIB}")
    set(GST_AIBOX_FRAME_META_LIB "$ENV{AIBOX_FRAMEMETA_LIB}")
    get_filename_component(GST_AIBOX_FRAME_META_DIR "${GST_AIBOX_FRAME_META_LIB}" DIRECTORY)
else()
    find_library(GST_AIBOX_FRAME_META_LIB NAMES gstaiboxframemeta
        PATHS
            ${CMAKE_CURRENT_LIST_DIR}/build_board
            ${CMAKE_CURRENT_LIST_DIR}/build
            ${CMAKE_SOURCE_DIR}/../aiboxframemeta_common/build_board
            ${CMAKE_SOURCE_DIR}/../aiboxframemeta_common/build
        NO_DEFAULT_PATH)
    if(GST_AIBOX_FRAME_META_LIB)
        get_filename_component(GST_AIBOX_FRAME_META_DIR "${GST_AIBOX_FRAME_META_LIB}" DIRECTORY)
    else()
        set(GST_AIBOX_FRAME_META_LIB gstaiboxframemeta)
    endif()
endif()
if(NOT GST_AIBOX_FRAME_META_DIR)
    set(GST_AIBOX_FRAME_META_DIR "${CMAKE_CURRENT_LIST_DIR}/build_board")
endif()
