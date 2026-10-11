#!/bin/sh
# Fallback when the headset has no C compiler: builds an aarch64 gaze_probe on a PC
# with podman or docker (Debian bookworm cross gcc, needs glibc >= 2.34 on the target).
# Run from Git Bash, WSL or Linux. Output: tools/gaze-probe/gaze_probe (aarch64).
set -eu
cd "$(dirname "$0")"
ENGINE="${ENGINE:-$(command -v podman || command -v docker)}"
SRC="$(pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 "$ENGINE" run --rm --arch amd64 -v "$SRC:/src" docker.io/library/debian:bookworm sh -c '
  set -e
  apt-get update -qq >/dev/null
  apt-get install -y -qq gcc-aarch64-linux-gnu libc6-dev-arm64-cross curl ca-certificates file >/dev/null
  cd /src && rm -f gaze_probe && CC=aarch64-linux-gnu-gcc ./build.sh && file gaze_probe'
