set(MEDIA_SNAP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(media_snapshot_event_id_reuse_tests
    unit/event_id_reuse_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/detection/FrameEventBuilder.cpp
    ${MEDIA_SNAP_ROOT}/src/detection/Uuid.cpp
)

target_include_directories(media_snapshot_event_id_reuse_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
)

target_link_libraries(media_snapshot_event_id_reuse_tests PRIVATE
    imsdk_proto
    ${UUID_LIBRARIES}
    Threads::Threads
)

add_test(NAME media_snapshot_event_id_reuse_tests
         COMMAND media_snapshot_event_id_reuse_tests)

add_executable(media_snapshot_encoder_tests
    unit/snapshot_encoder_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/SnapshotEncoder.cpp
)

target_include_directories(media_snapshot_encoder_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
)

target_link_libraries(media_snapshot_encoder_tests PRIVATE
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    Threads::Threads
)

add_test(NAME media_snapshot_encoder_tests COMMAND media_snapshot_encoder_tests)
set_tests_properties(media_snapshot_encoder_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)

add_executable(media_snapshot_thumbnail_encoder_tests
    unit/thumbnail_encoder_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/ThumbnailEncoder.cpp
)

target_include_directories(media_snapshot_thumbnail_encoder_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
)

target_link_libraries(media_snapshot_thumbnail_encoder_tests PRIVATE
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    Threads::Threads
)

add_test(NAME media_snapshot_thumbnail_encoder_tests
         COMMAND media_snapshot_thumbnail_encoder_tests)
set_tests_properties(media_snapshot_thumbnail_encoder_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)

add_executable(media_snapshot_writer_tests
    unit/media_writer_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/MediaFileWriter.cpp
)

target_include_directories(media_snapshot_writer_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
)

add_test(NAME media_snapshot_writer_tests COMMAND media_snapshot_writer_tests)

add_executable(media_snapshot_media_ready_pub_tests
    integration/media_ready_pub_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/MediaFileWriter.cpp
    ${MEDIA_SNAP_ROOT}/src/media/NatsMediaReadyTransport.cpp
    ${MEDIA_SNAP_ROOT}/src/adapter/NatsAdapter.cpp
)

target_include_directories(media_snapshot_media_ready_pub_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/integration
    ${NATS_INCLUDE_DIRS}
)

target_link_libraries(media_snapshot_media_ready_pub_tests PRIVATE
    imsdk_proto
    ${NATS_LIBRARIES}
    Threads::Threads
)

add_test(NAME media_snapshot_media_ready_pub_tests
         COMMAND media_snapshot_media_ready_pub_tests)
set_tests_properties(media_snapshot_media_ready_pub_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)

add_executable(media_snapshot_trigger_tests
    unit/snapshot_trigger_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/SnapshotTriggerPolicy.cpp
)

target_include_directories(media_snapshot_trigger_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
)

add_test(NAME media_snapshot_trigger_tests COMMAND media_snapshot_trigger_tests)

add_executable(media_snapshot_service_tests
    unit/snapshot_service_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/AnomalySnapshotService.cpp
    ${MEDIA_SNAP_ROOT}/src/media/SnapshotTriggerPolicy.cpp
    ${MEDIA_SNAP_ROOT}/src/media/MediaFileWriter.cpp
)

target_include_directories(media_snapshot_service_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
    ${GST_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
)

target_link_libraries(media_snapshot_service_tests PRIVATE
    ${GST_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    ${GST_APP_LIBRARIES}
    Threads::Threads
)

add_test(NAME media_snapshot_service_tests COMMAND media_snapshot_service_tests)
set_tests_properties(media_snapshot_service_tests PROPERTIES TIMEOUT 30)

add_executable(media_snapshot_event_id_cross_tests
    unit/event_id_cross_tests.cpp
    ${MEDIA_SNAP_ROOT}/src/media/AnomalySnapshotService.cpp
    ${MEDIA_SNAP_ROOT}/src/media/SnapshotTriggerPolicy.cpp
    ${MEDIA_SNAP_ROOT}/src/media/MediaFileWriter.cpp
    ${MEDIA_SNAP_ROOT}/src/detection/FrameEventBuilder.cpp
    ${MEDIA_SNAP_ROOT}/src/detection/Uuid.cpp
)

target_include_directories(media_snapshot_event_id_cross_tests PRIVATE
    ${MEDIA_SNAP_ROOT}/include
    ${GST_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
)

target_link_libraries(media_snapshot_event_id_cross_tests PRIVATE
    imsdk_proto
    ${GST_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${UUID_LIBRARIES}
    Threads::Threads
)

add_test(NAME media_snapshot_event_id_cross_tests
         COMMAND media_snapshot_event_id_cross_tests)
set_tests_properties(media_snapshot_event_id_cross_tests PROPERTIES TIMEOUT 30)

add_executable(media_ready_subscriber
    integration/media_ready_subscriber.cpp
    ${MEDIA_SNAP_ROOT}/src/adapter/NatsAdapter.cpp
)

target_include_directories(media_ready_subscriber PRIVATE
    ${MEDIA_SNAP_ROOT}/include
    ${NATS_INCLUDE_DIRS}
)

target_link_libraries(media_ready_subscriber PRIVATE
    imsdk_proto
    ${NATS_LIBRARIES}
    Threads::Threads
)
