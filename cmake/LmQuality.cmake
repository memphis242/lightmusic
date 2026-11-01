# Code-quality settings: warnings (lm_warnings_app / lm_warnings_test, our code only), -Werror policy,
# sanitizers and hardening. See CLAUDE.md for the policy behind these choices.

# --- -Werror policy --------------------------------------------------------------
# Off for development (Debug) builds so warnings never block iteration; on for
# optimised/release builds and in CI (which passes -DLM_WERROR=ON explicitly).
# Never applied to third_party (it gets -w and is included as SYSTEM).
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set(_lm_werror_default ON)
else()
  set(_lm_werror_default OFF)
endif()
option(LM_WERROR "Treat warnings in our own code as errors" ${_lm_werror_default})

set(_lm_gcc   $<CXX_COMPILER_ID:GNU>)
set(_lm_clang $<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>)

# --- Warnings (our code only) ----------------------------------------------------
# Two sets (see CLAUDE.md, "App vs test warning sets"):
#  lm_warnings_app   every library, server, frontend and tool target: the strictest set.
#  lm_warnings_test  unit-test executables and test-only helpers: the shared base set,
#                    without the app-only extras below.
# A warning whose nuisance hits are only in tests is turned off for tests only, never
# for app code. Both sets get -Werror under LM_WERROR.
set(_lm_warn_common
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
  -Wcast-qual -Wcast-align -Wformat=2 -Wundef
  -Wimplicit-fallthrough -Wmissing-declarations -Wvla
  -Wunused-result)
set(_lm_warn_cxx
  -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wextra-semi
  -Wsuggest-override -Wzero-as-null-pointer-constant)
# gcc-only analyses
set(_lm_warn_gcc
  -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wcast-align=strict
  -Wredundant-decls)
# clang-only analyses
set(_lm_warn_clang
  -Wshadow-all -Wcomma -Wunreachable-code-aggressive -Wloop-analysis
  -Wimplicit-int-float-conversion -Wmissing-prototypes -Wmissing-variable-declarations
  -Wnewline-eof -Wextra-semi-stmt -Wthread-safety -Wnull-dereference)
# App-only (gcc): -Wnull-dereference. In tests it reports paths inlined from libstdc++
# (std::streambuf::gptr()/egptr() via istreambuf_iterator in test file readers) that our
# code cannot change; app code has no such hits, so it is on there.
set(_lm_warn_gcc_app -Wnull-dereference)
# Not enabled in either set:
#  gcc -Wuseless-cast: every hit (21 in app code with gcc 13) is a cast between types
#   that are only identical on LP64 glibc and are needed elsewhere: uint64_t ring
#   positions -> size_t (32-bit size_t), time_t/off_t -> int64_t (32-bit time_t/off_t),
#   off_t -> long for libvorbisfile (32-bit long on Windows), mode_t -> unsigned for
#   printf. Dropping them breaks -Wconversion on those targets (Windows port planned).

function(_lm_add_warnings target)
  target_compile_options(${target} INTERFACE
    ${_lm_warn_common}
    "$<$<COMPILE_LANGUAGE:CXX>:${_lm_warn_cxx}>"
    "$<${_lm_gcc}:${_lm_warn_gcc}>"
    "$<${_lm_clang}:${_lm_warn_clang}>"
    $<$<BOOL:${LM_WERROR}>:-Werror>)
endfunction()

add_library(lm_warnings_app INTERFACE)
_lm_add_warnings(lm_warnings_app)
target_compile_options(lm_warnings_app INTERFACE "$<${_lm_gcc}:${_lm_warn_gcc_app}>")

add_library(lm_warnings_test INTERFACE)
_lm_add_warnings(lm_warnings_test)

# --- Sanitizers (unit-test builds) -------------------------------------------------
# UBSan extras beyond -fsanitize=undefined. Failures abort (-fno-sanitize-recover=all)
# so ctest sees them.
set(_lm_ubsan_extra float-divide-by-zero,float-cast-overflow)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # implicit-integer-truncation: a value changed by an implicit narrowing at runtime.
  # The rest of implicit-conversion (sign-change) is not enabled: it fires inside
  # libstdc++ headers (char <-> unsigned char in std::string/charconv), which we cannot
  # change; our own code is covered statically by -Wsign-conversion.
  # unsigned-integer-overflow is not enabled either: hashing/PRNG/ring indices wrap on
  # purpose (well-defined) and it reports those.
  string(APPEND _lm_ubsan_extra ",implicit-integer-truncation,local-bounds,nullability")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(APPEND _lm_ubsan_extra ",bounds-strict")
endif()

if(LM_ASAN)
  add_compile_options(-fsanitize=address,undefined,${_lm_ubsan_extra}
                      -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined,${_lm_ubsan_extra})
endif()
if(LM_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# --- Runtime checks / hardening --------------------------------------------------
# libstdc++ precondition checks (bounds on operator[], front() on empty, ...) in every
# non-release build: cheap, and catches bugs the sanitizers do not.
add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
# _GLIBCXX_DEBUG (checked iterators) is deliberately not offered: it changes the layout
# of std containers, and yaml-cpp, TagLib and FTXUI pass them across their (uninstrumented)
# library boundaries, so a mixed build is an ODR violation, not a stricter test.
# Debug: initialise automatic variables with a pattern so uninitialised reads are
# visible instead of "working by luck" (not with MSan-like tools; harmless with ASan).
add_compile_options($<$<CONFIG:Debug>:-ftrivial-auto-var-init=pattern>)
if(LM_HARDENING AND NOT LM_ASAN AND NOT LM_TSAN)
  add_compile_options(
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE>
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-D_FORTIFY_SOURCE=3>
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fstack-protector-strong>
    $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fstack-clash-protection>)
endif()
