# aggregator_nats tests

Independent short tests for IR-DATA-001 `detection.frame`.
This directory can be deleted without affecting the production binary
when `BUILD_TESTING` is OFF.

## Layout

- `unit/` contract tests: protobuf mapping, empty frame, UUID v4, WaitPopNext, fake transport, UUID throughput (NFR-4)
- `integration/aggregator_nats_integration_tests.cpp`: isolated `nats-server` on port 14223, 100-frame loopback, NFR-1 no-backlog
- `integration/nfr_reconnect_tests.cpp`: isolated port 14224, NFR-2 reconnect resume + NFR-3 Connect/Disconnect x20
- `integration/frame_event_subscriber.cpp`: standalone decoder for a live NATS subject
- `scripts/run_integration.sh`: board build + ctest helper

These tests do not start WebRTC and do not require a browser.

## Board commands

```bash
cmake -S . -B build_events \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_WEBRTC=OFF \
  -DENABLE_DETECTION_NATS=ON \
  -DBUILD_TESTING=ON
cmake --build build_events -j
ctest --test-dir build_events -R aggregator_nats --output-on-failure
```

Or:

```bash
bash test/aggregator_nats/scripts/run_integration.sh
```

If `nats-server` is missing, the integration test exits 77 and CTest reports skipped.

## Live subscriber (Phase 6)

```bash
./build_events/frame_event_subscriber \
  --url nats://127.0.0.1:4222 \
  --seconds 8 \
  --frame-width 1920 \
  --frame-height 1080
```
