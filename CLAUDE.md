# lightmusic: working notes for Claude

C++20 music player (libplayer, libconfig, libnet, lightmusicd server, FTXUI TUI). Target
platform: Fedora 43 (gcc 15, clang 21). See README.md for the user-facing docs.

## Coding principles

1. **Let the compilers analyse the code.** `lm_warnings` (cmake/LmQuality.cmake) enables a
   wide warning set (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
   -Wold-style-cast ...` plus gcc-only and clang-only extras). New code must build
   warning-free on both compilers. Fix the cause; do not cast or pragma a warning away
   without understanding it. A justified suppression is narrow (one line/one cast) and has
   a comment saying why.
2. **One primary compiler, others as checkers.** gcc (Fedora's default) is used for debug,
   unit-test and release builds. clang is an extra checking layer (its own warnings,
   clang-only UBSan checks, clang-tidy). MSVC joins as a third layer when Windows
   portability arrives (planned ~v3); not implemented yet.
3. **-Werror only where warnings must already be fixed.** `LM_WERROR` defaults ON for
   Release/RelWithDebInfo/MinSizeRel and OFF for Debug; CI always passes `-DLM_WERROR=ON`.
   Warnings may exist while developing, never in a PR. Never applied to `third_party/`
   (built with `-w`, included as SYSTEM).
4. **Run the tests under sanitizers.** ASan+UBSan (with extra UBSan checks, and
   `-fno-sanitize-recover=all` so a finding fails the test) and TSan. Debug builds also get
   `_GLIBCXX_ASSERTIONS` and `-ftrivial-auto-var-init=pattern`; optimised builds get
   `_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, `-fstack-clash-protection`.
5. **Two static analysers with different methods**: clang-tidy (AST checks plus the clang
   static analyzer's symbolic execution via `clang-analyzer-*`) and cppcheck (data-flow).
   Real findings are fixed; false positives are suppressed inline
   (`// NOLINT(check): reason`, `// cppcheck-suppress id` + reason), never wholesale.
6. **Assertions** (`include/lm/assert.hpp`): `static_assert` for anything knowable at
   compile time; `LM_ASSERT(cond, msg)` for internal invariants (pre/postconditions,
   data-structure consistency); `LM_ASSERT_RT(cond)` inside the realtime audio callback
   (no I/O, traps). Active without `NDEBUG` or with `-DLM_ASSERTS=ON`; keep conditions
   side-effect free. Never assert on untrusted input: audio files/tags, network data and
   config get proper error handling in every build.
7. **Behaviour stays unchanged when fixing warnings**: run the full test suite after each
   batch of fixes.
8. Do not modify `third_party/` (vendored; see third_party/VERSIONS.md).

Decisions recorded:
- `-Wuseless-cast` (gcc) is off: it flags `int64_t`/`off_t`/`long long` casts that are
  no-ops on x86-64 Linux but needed for printf formats and 32-bit/Windows portability.
- gcc's `-Wnull-dereference` is off (clang's is on): at -O2 gcc reports paths inlined from
  libstdc++ (`streambuf::gptr`) that cannot be fixed in our code.
- UBSan `unsigned-integer-overflow` (clang) is off: unsigned wrap is well defined and used
  on purpose (hashes, PRNGs, ring-buffer indices).
- MSan is not used: it needs every linked library (libstdc++, opusfile, vorbis, curl,
  sqlite, yaml-cpp, TagLib, sd-bus) built with MSan, otherwise it reports false positives
  on every value they return. ASan+UBSan, `-ftrivial-auto-var-init=pattern`,
  `-Wuninitialized`/`-Wmaybe-uninitialized` and the analysers cover uninitialised reads.
- `_GLIBCXX_DEBUG` is not used: it changes std container layout, and yaml-cpp, TagLib and
  FTXUI pass containers across their uninstrumented interfaces (ODR violation).
- Valgrind (memcheck/helgrind) is not in CI: ASan and TSan find the same classes of bugs
  10-50x faster, and helgrind does not understand the C++ atomics the lock-free ring
  uses (false positives). It remains useful by hand on an un-sanitized Debug build for a
  suspected leak/race (`valgrind --tool=memcheck build/tests/test_player`).

## Commands

```sh
# Primary (gcc) development build; warnings shown, not fatal
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build && ctest --test-dir build --output-on-failure

# What CI requires: -Werror, gcc and clang
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLM_WERROR=ON
CC=clang CXX=clang++ cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLM_WERROR=ON

# Sanitizers (unit-test builds)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLM_ASAN=ON
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLM_TSAN=ON
CC=clang CXX=clang++ cmake -S . -B build-casan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLM_ASAN=ON
# (then ninja -C <dir> && ctest --test-dir <dir> --output-on-failure)

# Static analysis (same scripts CI runs; inside fedora:43 they also install the tools)
CC=clang CXX=clang++ cmake -S . -B build-analyze -G Ninja -DCMAKE_BUILD_TYPE=Debug
run-clang-tidy -p build-analyze -quiet '^(?!.*(/third_party/|/_deps/)).*\.cpp$'
cppcheck --project=build-analyze/compile_commands.json --enable=warning,performance,portability \
  --std=c++20 --inline-suppr --suppressions-list=.cppcheck-suppressions -i third_party \
  -i build-analyze/_deps --library=posix --check-level=exhaustive --error-exitcode=1 --quiet

# Reproduce a CI job exactly
docker run --rm -v "$PWD:/src" -w /src -e LM_CC=clang fedora:43 \
  .github/ci/build-test.sh RelWithDebInfo -DLM_WERROR=ON
docker run --rm -v "$PWD:/src" -w /src fedora:43 .github/ci/analyze.sh clang-tidy
```

CI (.github/workflows/ci.yml, fedora:43 containers, parallel jobs): normal (gcc
RelWithDebInfo with `-DLM_ASSERTS=ON`), release (gcc Release -O3, hardening), asan-ubsan
(gcc), tsan (gcc), net-off (gcc), clang, clang-asan-ubsan, clang-tidy, cppcheck. All must
be green before merging.
