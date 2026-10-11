#!/bin/sh
# Builds gaze_probe on the Steam Frame (or any Linux box). Needs cc and curl.
# The OpenXR headers are downloaded once into ./openxr-headers. No OpenXR loader,
# CMake, GL or EGL development packages are needed.
set -eu
cd "$(dirname "$0")"
OPENXR_TAG=release-1.1.62
BASE="https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/$OPENXR_TAG/include/openxr"
mkdir -p openxr-headers/openxr
for h in openxr.h openxr_platform.h openxr_platform_defines.h openxr_loader_negotiation.h; do
  if [ ! -s "openxr-headers/openxr/$h" ]; then
    echo "Downloading $h ($OPENXR_TAG)"
    curl -fsSL -o "openxr-headers/openxr/$h" "$BASE/$h"
  fi
done
CC="${CC:-cc}"
"$CC" -std=gnu11 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-format-truncation \
  -I openxr-headers -o gaze_probe gaze_probe.c -ldl -lm
echo "Built $(pwd)/gaze_probe"
