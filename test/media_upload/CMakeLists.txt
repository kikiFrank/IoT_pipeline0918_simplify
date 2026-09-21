set(MEDIA_UPLOAD_TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(upload_injector
    upload_injector.cpp
    ${MEDIA_UPLOAD_TEST_ROOT}/src/adapter/NatsAdapter.cpp
)
target_include_directories(upload_injector PRIVATE
    ${MEDIA_UPLOAD_TEST_ROOT}/include
    ${NATS_INCLUDE_DIRS}
)
target_link_libraries(upload_injector PRIVATE
    ${NATS_LIBRARIES}
    Threads::Threads
)

add_executable(upload_verifier
    upload_verifier.cpp
    ${MEDIA_UPLOAD_TEST_ROOT}/src/adapter/NatsAdapter.cpp
)
target_include_directories(upload_verifier PRIVATE
    ${MEDIA_UPLOAD_TEST_ROOT}/include
    ${NATS_INCLUDE_DIRS}
)
target_link_libraries(upload_verifier PRIVATE
    ${NATS_LIBRARIES}
    Threads::Threads
)
