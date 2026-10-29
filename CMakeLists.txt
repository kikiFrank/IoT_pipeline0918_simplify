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
include(CTest)

# --- NATS / detection.frame / annotated RTSP data plane ---
# Deprecated: legacy P2P WebRTC was removed. Accepted for script compatibility.
option(ENABLE_WEBRTC "Deprecated: ignored; P2P WebRTC has been removed" OFF)
option(ENABLE_DETECTION_NATS "Build IR-DATA-001 detection.frame Protobuf/NATS publisher" OFF)
option(ENABLE_ANNOTATED_RTSP "Build annotated H.264 RTSP server output" OFF)
option(ENABLE_SIGLIP_WORKER "Build the independent SigLIP2 image Worker" OFF)

if(ENABLE_SIGLIP_WORKER AND NOT ENABLE_DETECTION_NATS)
    message(FATAL_ERROR "ENABLE_SIGLIP_WORKER requires ENABLE_DETECTION_NATS=ON")
endif()

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
        src/media/AnomalySnapshotService.cpp
        src/media/SnapshotRequestSubscriber.cpp)
endif()

if(ENABLE_ANNOTATED_RTSP)
    list(APPEND MAIN_SOURCES src/media/AnnotatedRtspServer.cpp)
endif()

add_library(aibox_frame_cache STATIC src/media/FrameSnapshotCache.cpp)
target_include_directories(aibox_frame_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aibox_frame_cache PUBLIC Threads::Threads)

add_executable(aibox_stream_pipeline_demo ${MAIN_SOURCES})

target_include_directories(aibox_stream_pipeline_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${UUID_INCLUDE_DIRS}
)

set(MAIN_LIBS
    aibox_frame_cache
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

if(ENABLE_SIGLIP_WORKER)
    pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)
    pkg_check_modules(JPEG REQUIRED libjpeg)
    pkg_check_modules(PNG REQUIRED libpng)
    find_package(OpenSSL REQUIRED)
    add_library(aibox_siglip_core STATIC
        src/config/IniParser.cpp
        src/siglip_worker/SiglipConfigLoader.cpp
        src/siglip_worker/JsonUtil.cpp
        src/siglip_worker/EmbeddingJson.cpp
        src/siglip_worker/ImagePreprocess.cpp
        src/siglip_worker/EmbeddingOutbox.cpp)
    target_include_directories(aibox_siglip_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${JSONGLIB_INCLUDE_DIRS}
        ${JPEG_INCLUDE_DIRS}
        ${PNG_INCLUDE_DIRS})
    target_link_libraries(aibox_siglip_core PUBLIC
        ${JSONGLIB_LIBRARIES} ${JPEG_LIBRARIES} ${PNG_LIBRARIES} OpenSSL::Crypto)
    add_executable(aibox_siglip_worker
        src/siglip_worker/main.cpp
        src/siglip_worker/SiglipWorker.cpp
        src/siglip_worker/SiglipNatsClient.cpp
        src/siglip_worker/AlarmTaskConsumer.cpp
        src/siglip_worker/SiglipPipelineBuilder.cpp
        src/pipeline/GstElementHelper.cpp)
    target_include_directories(aibox_siglip_worker PRIVATE
        ${GST_INCLUDE_DIRS} ${GST_APP_INCLUDE_DIRS} ${NATS_INCLUDE_DIRS})
    target_link_libraries(aibox_siglip_worker PRIVATE
        aibox_siglip_core ${GST_LIBRARIES} ${GST_APP_LIBRARIES}
        ${NATS_LIBRARIES} Threads::Threads)
    if(BUILD_TESTING)
        target_compile_definitions(aibox_siglip_worker PRIVATE SIGLIP_TEST_HOOKS=1)
        add_executable(siglip_preprocess_probe tests/siglip_worker/preprocess_probe.cpp)
        target_link_libraries(siglip_preprocess_probe PRIVATE aibox_siglip_core)
        add_executable(siglip_isolated_harness tests/siglip_worker/isolated_harness.cpp)
        target_include_directories(siglip_isolated_harness PRIVATE ${NATS_INCLUDE_DIRS})
        target_link_libraries(siglip_isolated_harness PRIVATE aibox_siglip_core ${NATS_LIBRARIES})
        add_executable(siglip_hardware_benchmark
            tests/siglip_worker/hardware_benchmark.cpp
            src/siglip_worker/SiglipPipelineBuilder.cpp
            src/pipeline/GstElementHelper.cpp)
        target_include_directories(siglip_hardware_benchmark PRIVATE ${GST_INCLUDE_DIRS} ${GST_APP_INCLUDE_DIRS})
        target_link_libraries(siglip_hardware_benchmark PRIVATE
            aibox_siglip_core ${GST_LIBRARIES} ${GST_APP_LIBRARIES} Threads::Threads)
    endif()
endif()

# --- frame cache unit test ---
if(BUILD_TESTING)
    add_executable(frame_cache_policy_test tests/frame_cache_policy_test.cpp)
    target_link_libraries(frame_cache_policy_test PRIVATE aibox_frame_cache)
    add_test(NAME frame_cache_policy_test COMMAND frame_cache_policy_test)
    add_executable(snapshot_service_test
        tests/snapshot_service_test.cpp
        src/media/AnomalySnapshotService.cpp
        src/media/MediaFileWriter.cpp
        src/media/SnapshotTriggerPolicy.cpp)
    target_include_directories(snapshot_service_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${GST_INCLUDE_DIRS} ${GST_VIDEO_INCLUDE_DIRS})
    target_link_libraries(snapshot_service_test PRIVATE
        aibox_frame_cache ${GST_LIBRARIES} ${GST_VIDEO_LIBRARIES} Threads::Threads)
    add_test(NAME snapshot_service_test COMMAND snapshot_service_test)
    add_test(NAME frame_cache_config_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_frame_cache_config.sh
                $<TARGET_FILE:aibox_stream_pipeline_demo>
                ${CMAKE_CURRENT_SOURCE_DIR}/config_snapshots/demo_2stage_yolo_serial/pipeline_otm_12cam_webrtc.txt
    )
    if(ENABLE_SIGLIP_WORKER)
        add_executable(siglip_worker_core_test tests/siglip_worker/core_test.cpp)
        target_link_libraries(siglip_worker_core_test PRIVATE aibox_siglip_core)
        add_test(NAME siglip_worker_core_test COMMAND siglip_worker_core_test)
    endif()
endif()

# --- install ---
# The P2P WebRTC harness targets were removed with the test/ tree; the
# FrameSnapshotCache test above has no GStreamer dependency.
install(TARGETS aibox_stream_pipeline_demo
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
