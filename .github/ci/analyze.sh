#!/usr/bin/env bash
# Static analysis of our own code (never third_party/ or fetched deps). Runs inside fedora:43.
# Usage: analyze.sh clang-tidy|cppcheck
set -euo pipefail
tool=$1

dnf -y -q install --setopt=install_weak_deps=False \
  clang clang-tools-extra cppcheck cmake ninja-build pkgconf-pkg-config git \
  libcurl-devel opusfile-devel libvorbis-devel libogg-devel flac-devel mpg123-devel \
  ncurses-devel ftxui-devel systemd-devel cpp-httplib-devel taglib-devel sqlite-devel \
  yaml-cpp-devel

git config --global --add safe.directory '*'
# Configure with clang so compile_commands.json holds flags clang-tidy understands.
CC=clang CXX=clang++ cmake -S . -B build-analyze -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLM_FETCH_FTXUI=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Our translation units: everything in the compilation database outside third_party/_deps.
src_regex='^(?!.*(/third_party/|/_deps/)).*\.cpp$'

case "$tool" in
  clang-tidy)
    clang-tidy --version
    run-clang-tidy -p build-analyze -j "$(nproc)" -quiet "$src_regex"
    ;;
  cppcheck)
    cppcheck --version
    cppcheck --project=build-analyze/compile_commands.json \
      --enable=warning,performance,portability --std=c++20 \
      --inline-suppr --suppressions-list=.cppcheck-suppressions \
      -i third_party -i build-analyze/_deps --library=posix \
      --check-level=exhaustive --error-exitcode=1 -j "$(nproc)" --quiet
    ;;
  *) echo "unknown tool: $tool" >&2; exit 2 ;;
esac
