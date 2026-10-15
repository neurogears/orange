#!/usr/bin/env bash
# Build and run the synthetic SyncPulse test (no cameras needed; needs
# libzmq3-dev). ZMQ_INC / ZMQ_LIB override where zmq.h / libzmq.so are.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/sync_pulse_test
zmq_flags=()
[[ -n "${ZMQ_INC:-}" ]] && zmq_flags+=(-I "$ZMQ_INC")
[[ -n "${ZMQ_LIB:-}" ]] && zmq_flags+=(-L "$ZMQ_LIB")
g++ -O2 -std=c++17 -I src "${zmq_flags[@]}" \
    tests/sync_pulse_test.cpp src/sync_pulse.cpp -lzmq -lpthread -o "$out"
"$out"
