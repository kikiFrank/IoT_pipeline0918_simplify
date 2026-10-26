cmake_minimum_required(VERSION 3.16)
project(aibox_stream_pipeline_demo VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST       REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0>=1.16)
pkg_check_modules(GST_APP   REQUIRED gstreamer-app-1.0>=1.16)
pkg_check_modules(UUID      REQUIRED uuid)

find_package(Threads REQUIRED)

# --- NATS / detection.frame / annotated RTSP data plane ---
# Deprecated: legacy P2P WebRTC was removed. Accepted for script compatibility.
option(ENABLE_WEBRTC "Deprecated: ignored; P2P WebRTC has been removed" OFF)
option(ENABLE_DETECTION_NATS "Build IR-DATA-001 detection.frame Protobuf/NATS publisher" OFF)
option(ENABLE_ANNOTATED_RTSP "Build annotated H.264 RTSP server output" OFF)

if(ENABLE_WEBRTC)
    message(WARNING "ENABLE_WEBRTC is ignored: legacy P2P WebRTC has been removed")
endif()

if(ENABLE_DETECTION_NATS)
    pkg_check_modules(NATS REQUIRED libnats)
endif()

if(ENABLE_DETECTION_NATS)
    find_package(Protobuf REQUIRED)
    set(IMSDK_CONTRACT_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/contracts/imsdk/1.0/aibox_contracts.proto)
    protobuf_generate_cpp(IMSDK_PROTO_SRCS IMSDK_PROTO_HDRS ${IMSDK_CONTRACT_PROTO})
    add_library(imsdk_proto STATIC ${IMSDK_PROTO_SRCS} ${IMSDK_PROTO_HDRS})
    target_include_directories(imsdk_proto PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(imsdk_proto PUBLIC ${Protobuf_LIBRARIES})
endif()

if(ENABLE_ANNOTATED_RTSP)
    pkg_check_modules(GST_RTSP_SERVER REQUIRED gstreamer-rtsp-server-1.0>=1.16)
endif()

# --- main executable ---
set(MAIN_SOURCES
    main.cpp
    src/config/IniParser.cpp
    src/config/TxtSnapshotLoader.cpp
    src/orchestrator/PipelineOrchestrator.cpp
    src/orchestrator/PlanExporter.cpp
    src/pipeline/PipelineStructureDumper.cpp
    src/pipeline/FaceOcOverlay.cpp
    src/pipeline/GstElementHelper.cpp
    src/pipeline/GstLinkHelper.cpp
    src/pipeline/GstPipelineBuilder.cpp
    src/pipeline/GstDotDumper.cpp
    src/pipeline/AppSinkCallbacks.cpp
    src/aggregator/ResultAggregator.cpp
    src/publisher/EventBuilder.cpp
    src/publisher/JsonFileEventSink.cpp
    src/publisher/DetectionEventPublisher.cpp
)

if(ENABLE_DETECTION_NATS)
    list(APPEND MAIN_SOURCES
        src/adapter/NatsAdapter.cpp
        src/detection/Uuid.cpp
        src/detection/CaptureTimestampTracker.cpp
        src/detection/FrameEventBuilder.cpp
        src/detection/FrameEventPublisher.cpp
        src/detection/NatsFrameTransport.cpp
        src/media/SnapshotEncoder.cpp
        src/media/ThumbnailEncoder.cpp
        src/media/MediaFileWriter.cpp
        src/media/NatsMediaReadyTransport.cpp
        src/media/SnapshotTriggerPolicy.cpp
        src/media/AnomalySnapshotService.cpp)
endif()

if(ENABLE_ANNOTATED_RTSP)
    list(APPEND MAIN_SOURCES src/media/AnnotatedRtspServer.cpp)
endif()

add_executable(aibox_stream_pipeline_demo ${MAIN_SOURCES})

target_include_directories(aibox_stream_pipeline_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${UUID_INCLUDE_DIRS}
)

set(MAIN_LIBS
    ${GST_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${UUID_LIBRARIES}
    Threads::Threads
    dl
)

if(ENABLE_DETECTION_NATS)
    list(APPEND MAIN_LIBS ${NATS_LIBRARIES} imsdk_proto)
    target_include_directories(aibox_stream_pipeline_demo PRIVATE
        ${NATS_INCLUDE_DIRS}
        ${Protobuf_INCLUDE_DIRS}
        ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(aibox_stream_pipeline_demo PRIVATE HAVE_DETECTION_NATS=1)
endif()

if(ENABLE_ANNOTATED_RTSP)
    list(APPEND MAIN_LIBS ${GST_RTSP_SERVER_LIBRARIES})
    target_include_directories(aibox_stream_pipeline_demo PRIVATE ${GST_RTSP_SERVER_INCLUDE_DIRS})
    target_compile_definitions(aibox_stream_pipeline_demo PRIVATE HAVE_ANNOTATED_RTSP=1)
endif()

target_link_libraries(aibox_stream_pipeline_demo ${MAIN_LIBS})

# --- install ---
# Unit-test and P2P WebRTC harness targets were removed with the test/ tree;
# the runtime acceptance surface is the annotated RTSP server on :8555.
install(TARGETS aibox_stream_pipeline_demo
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
