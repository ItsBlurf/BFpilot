#!/usr/bin/env bash
set -e
export PS5_PAYLOAD_SDK="C:/Users/Blurf/ps5dev/toolchains/ps5-payload-sdk"
make clean && make -j4
