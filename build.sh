#!/usr/bin/env bash
set -euo pipefail
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK to your PS5 payload SDK directory first}"
cd -- "$(dirname -- "$0")"
if [[ ! -f "$PS5_PAYLOAD_SDK/toolchain/prospero.mk" ]]; then
  echo 'PS5_PAYLOAD_SDK does not contain toolchain/prospero.mk.' >&2
  exit 2
fi
if (($#)); then
  exec make "$@"
fi
exec make all
