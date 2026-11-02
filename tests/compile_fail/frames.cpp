// Compile-fail cases for lm::Frames (tests/CMakeLists.txt builds this once per
// LM_CF_CASE and requires the build to fail naming lm::Frames).
#include "lm/decoder.hpp"

#include <cstdint>

void lm_compile_fail_case(lm::Decoder &d);
void lm_compile_fail_case(lm::Decoder &d) {
#if LM_CF_CASE == 1 // a bare integer is not a frame count: Decoder::seek(lm::Frames{n})
    const std::uint64_t frame = 42;
    (void)d.seek(frame);
#elif LM_CF_CASE == 2 // nor does a frame count turn back into one implicitly: use count()
    const std::uint64_t n = d.totalFrames();
    (void)n;
#else
#error "LM_CF_CASE not set"
#endif
}
