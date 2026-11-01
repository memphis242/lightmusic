# lightmusic: working notes for Claude

C++23 music player (libplayer, libconfig, libnet, lightmusicd server, FTXUI TUI). Target
platform: Fedora 43 (gcc 15, clang 21). See README.md for the user-facing docs.

## Coding principles

1. **Let the compilers analyse the code.** `lm_warnings_app`/`lm_warnings_test` (cmake/LmQuality.cmake) enables a
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
6. **App vs test warning sets.** App code (libplayer, libnet, libconfig, server,
   frontends, tools, include) uses the strictest set: `lm_warnings_app`, the root
   `.clang-tidy`, the global cppcheck config. Tests and test-only helpers use
   `lm_warnings_test`, `tests/.clang-tidy` (inherits the root and relaxes it) and
   suppressions scoped to `tests/`. Unit tests may have much lower warning standards;
   **never disable an app check because of nuisance hits in tests**: relax it for tests
   only. A check stays off for app code only when its reason applies to app code (e.g.
   hits inside libstdc++ headers), with that reason written next to it. -Werror (CI)
   applies to both sets.
7. **Check every return value**, especially of system calls: handle the error, or, where
   ignoring it is truly correct (best-effort cleanup close or unlink), discard it explicitly with `(void)` plus a brief comment when not obvious.
   Our own functions whose result matters are `[[nodiscard]]`. Enforced by cert-err33-c,
   bugprone-unused-return-value (CheckedFunctions widened to POSIX/sqlite/curl calls in
   `.clang-tidy`) and `-Wunused-result`, in app and test code.
8. **Error handling.** Two kinds of failure. Untrusted/external input or environment
   (files, tags, network, server responses, config, user input, failing syscalls) is a
   recoverable error in every build, never assert/abort/UB; internal invariant violations
   are assertions (9). Module conventions: libplayer returns `bool`/`nullptr` with a
   `std::string *err` out-parameter (decoders also expose `lastError()`, surfaced by the
   player as events); libconfig returns `bool` + `std::string &err` and collects
   line-numbered errors; server and libnet's sqlite/curl wrappers throw
   `std::runtime_error`, caught at request/thread/command boundaries; TUI shows errors on
   the status line, CLI tools and the server on stderr/log.
   - Nothing fails silently: handle, propagate with context (operation + path/id +
     `strerror(errno)` or the library message), or report to the user. No empty catch
     blocks; deliberately ignored best-effort cleanup is `(void)`-discarded (7).
   - Fail closed when security-relevant (file permissions, key/credential storage, auth,
     TLS/plain-http warnings): refuse to start/serve rather than continue insecurely.
   - Degrade gracefully for non-essential features (MPRIS bus lost, metered probe
     unavailable, cover art missing): playback keeps working, log once.
   - Resource safety on error paths: RAII for fds/handles/sqlite/curl; a throwing
     constructor must not leak what it already acquired.
   - Exceptions never cross thread entry points, C callbacks (curl, sd-bus, miniaudio,
     decoder callbacks) or the audio callback: catch there and convert to the module's
     error reporting; the realtime callback reports only via atomics.
   - Error paths get tests (failure injection, fake servers, permission cases) like
     happy paths.
9. **Assertions** (`include/lm/assert.hpp`): `static_assert` for anything knowable at
   compile time; `LM_ASSERT(cond, msg)` for internal invariants (pre/postconditions,
   data-structure consistency); `LM_ASSERT_RT(cond)` inside the realtime audio callback
   (no I/O, traps). Active without `NDEBUG` or with `-DLM_ASSERTS=ON`; keep conditions
   side-effect free. Never assert on untrusted input: audio files/tags, network data and
   config get proper error handling in every build.
10. **Output** (`include/lm/print.hpp`, README "Output"): C++23 `std::format` /
   `std::print`, never printf/snprintf/puts/fputs/perror or iostreams in app code
   (clang-tidy: modernize-use-std-print, bugprone-unsafe-functions.CustomFunctions).
   `std::print` throws on a failed write, so: best-effort diagnostics use
   `lm::diag`/`lm::diagln` (failure swallowed and counted, noexcept); output that matters
   (keys, listings for scripts) uses `[[nodiscard]] lm::out`/`lm::outln` + `lm::flushOut`
   and fails loudly. Use `std::format_to`/`std::format_to_n` into an existing
   string/buffer to avoid extra allocations. Signal handlers, LM_ASSERT/LM_ASSERT_RT and
   the realtime audio callback keep `write(2)` or no I/O (formatting allocates, stdio
   locks). Tests may use `std::println` directly.
11. **Interfaces and constraints (concepts, strong typing).** C++26 Contracts
   (`pre`/`post`/`contract_assert`) are not available in gcc 15 / clang 21, so
   preconditions and postconditions stay `LM_ASSERT` (9). For "interface-like" compile-time
   requirements use C++20 **concepts**: constrain every template parameter with a standard
   concept (`std::integral`, `std::floating_point`, `std::invocable`, `std::copyable`,
   `std::ranges::range`, ...) or a small named project concept when it states a real
   requirement (e.g. `IntFieldValue`, `JsonScalar`), instead of `static_assert`/`enable_if`
   in the body, so misuse fails at the call site naming the requirement. Constraints that
   matter get a compile-fail test (`tests/compile_fail/`, `lm_compile_fail()` in
   tests/CMakeLists.txt). Runtime polymorphism stays virtual where it is today
   (ByteSource, Decoder: one call per buffer, and templates would grow the binaries):
   interfaces have pure virtual functions, a virtual destructor and deleted copy
   (owned through `unique_ptr`); implementations are `final` and every override says
   `override`. Strong types (distinct types for frames vs bytes vs seconds, track id vs
   index) only where a mix-up is plausible and the wrapper is cheap; no big refactors for
   them.
12. **Behaviour stays unchanged when fixing warnings**: run the full test suite after each
   batch of fixes.
13. Do not modify `third_party/` (vendored; see third_party/VERSIONS.md).

Decisions recorded:
- `-Wuseless-cast` (gcc) is off for app and tests: re-evaluated for app code, all hits
  are casts between types identical only on LP64 glibc (uint64_t ring positions ->
  size_t, time_t/off_t -> int64_t, off_t -> long for vorbisfile; the mode_t -> unsigned
  printf casts went away with std::format); they are required for 32-bit and Windows (planned) under -Wconversion.
- gcc's `-Wnull-dereference` is ON for app code (no hits) and off for tests only: there
  gcc reports paths inlined from libstdc++ (`streambuf::gptr/egptr` via
  istreambuf_iterator in test file readers). clang's is on for both.
- UBSan `unsigned-integer-overflow` (clang) is off: unsigned wrap is well defined and used
  on purpose (hashes, PRNGs, ring-buffer indices).
- clang UBSan `implicit-conversion` is limited to `implicit-integer-truncation`: the
  sign-change part fires inside libstdc++ headers (char <-> unsigned char), and our
  code is covered for sign changes statically by `-Wsign-conversion`.
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
run-clang-tidy -p build-analyze -quiet '^(?!.*(/third_party/|/_deps/|/compile_fail/)).*\.cpp$'
cppcheck --project=build-analyze/compile_commands.json --enable=warning,performance,portability \
  --std=c++23 --inline-suppr --suppressions-list=.cppcheck-suppressions -i third_party \
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
