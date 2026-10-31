#!/usr/bin/env bash
# Install Fedora deps, configure, build and test lightmusic. Runs inside fedora:43.
# Usage: build-test.sh <build-type> [extra cmake args...]
set -euo pipefail
build_type=$1; shift

dnf -y -q install --setopt=install_weak_deps=False \
  gcc-c++ cmake ninja-build pkgconf-pkg-config git \
  libcurl-devel opusfile-devel libvorbis-devel libogg-devel flac-devel mpg123-devel \
  ncurses-devel ftxui-devel systemd-devel cpp-httplib-devel taglib-devel sqlite-devel \
  yaml-cpp-devel ffmpeg-free util-linux util-linux-script dbus-daemon dbus-tools \
  playerctl procps-ng strace libasan libubsan libtsan

git config --global --add safe.directory '*'
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DLM_FETCH_FTXUI=OFF "$@"
cmake --build build

# Fail on any test failure; also fail if any test skipped (exit 77): every
# optional tool (ffmpeg, script, dbus-run-session, playerctl) is installed here.
set +e
ctest --test-dir build --output-on-failure 2>&1 | tee ctest.log
rc=${PIPESTATUS[0]}
set -e
skipped=$(grep -c '\*\*\*Skipped' ctest.log || true)
echo "Skipped tests: $skipped"
if [ "$skipped" -ne 0 ]; then
  grep '\*\*\*Skipped' ctest.log
  echo "::error::$skipped test(s) skipped in CI; all optional test tools are installed"
  exit 1
fi
exit "$rc"
