set(AGG_NATS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(aggregator_nats_contract_tests
    unit/aggregator_nats_contract_tests.cpp
    ${AGG_NATS_ROOT}/src/aggregator/ResultAggregator.cpp
    ${AGG_NATS_ROOT}/src/detection/FrameEventBuilder.cpp
    ${AGG_NATS_ROOT}/src/detection/FrameEventPublisher.cpp
    ${AGG_NATS_ROOT}/src/detection/CaptureTimestampTracker.cpp
    ${AGG_NATS_ROOT}/src/detection/Uuid.cpp
)

target_include_directories(aggregator_nats_contract_tests PRIVATE
    ${AGG_NATS_ROOT}/include
)

target_link_libraries(aggregator_nats_contract_tests PRIVATE
    imsdk_proto
    ${UUID_LIBRARIES}
    Threads::Threads
)

add_test(NAME aggregator_nats_contract_tests COMMAND aggregator_nats_contract_tests)

add_executable(aggregator_nats_integration_tests
    integration/aggregator_nats_integration_tests.cpp
    ${AGG_NATS_ROOT}/src/aggregator/ResultAggregator.cpp
    ${AGG_NATS_ROOT}/src/detection/FrameEventBuilder.cpp
    ${AGG_NATS_ROOT}/src/detection/FrameEventPublisher.cpp
    ${AGG_NATS_ROOT}/src/detection/NatsFrameTransport.cpp
    ${AGG_NATS_ROOT}/src/detection/Uuid.cpp
    ${AGG_NATS_ROOT}/src/adapter/NatsAdapter.cpp
)

target_include_directories(aggregator_nats_integration_tests PRIVATE
    ${AGG_NATS_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/integration
    ${NATS_INCLUDE_DIRS}
)

target_link_libraries(aggregator_nats_integration_tests PRIVATE
    imsdk_proto
    ${NATS_LIBRARIES}
    ${UUID_LIBRARIES}
    Threads::Threads
)

add_test(NAME aggregator_nats_integration_tests COMMAND aggregator_nats_integration_tests)
set_tests_properties(aggregator_nats_integration_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
)

add_executable(aggregator_nats_nfr_tests
    integration/nfr_reconnect_tests.cpp
    ${AGG_NATS_ROOT}/src/aggregator/ResultAggregator.cpp
    ${AGG_NATS_ROOT}/src/detection/FrameEventBuilder.cpp
    ${AGG_NATS_ROOT}/src/detection/FrameEventPublisher.cpp
    ${AGG_NATS_ROOT}/src/detection/NatsFrameTransport.cpp
    ${AGG_NATS_ROOT}/src/detection/Uuid.cpp
    ${AGG_NATS_ROOT}/src/adapter/NatsAdapter.cpp
)

target_include_directories(aggregator_nats_nfr_tests PRIVATE
    ${AGG_NATS_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/integration
    ${NATS_INCLUDE_DIRS}
)

target_link_libraries(aggregator_nats_nfr_tests PRIVATE
    imsdk_proto
    ${NATS_LIBRARIES}
    ${UUID_LIBRARIES}
    Threads::Threads
)

add_test(NAME aggregator_nats_nfr_tests COMMAND aggregator_nats_nfr_tests)
set_tests_properties(aggregator_nats_nfr_tests PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 60
)

add_executable(frame_event_subscriber
    integration/frame_event_subscriber.cpp
    ${AGG_NATS_ROOT}/src/adapter/NatsAdapter.cpp
)

target_include_directories(frame_event_subscriber PRIVATE
    ${AGG_NATS_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/integration
    ${NATS_INCLUDE_DIRS}
)

target_link_libraries(frame_event_subscriber PRIVATE
    imsdk_proto
    ${NATS_LIBRARIES}
    Threads::Threads
)
