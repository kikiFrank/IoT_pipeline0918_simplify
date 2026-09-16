#!/bin/bash
# Build and run IR-DATA-001 aggregator_nats tests on the board.
# Does not start WebRTC.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build_events}"
cd "$ROOT"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_WEBRTC=OFF \
    -DENABLE_DETECTION_NATS=ON \
    -DBUILD_TESTING=ON
fi

cmake --build "$BUILD" -j"${NPROC:-$(nproc)}" \
  --target aggregator_nats_contract_tests \
           aggregator_nats_integration_tests \
           aggregator_nats_nfr_tests \
           frame_event_subscriber

ctest --test-dir "$BUILD" -R aggregator_nats --output-on-failure
