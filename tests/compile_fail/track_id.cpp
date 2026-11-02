// Compile-fail cases for lm::TrackId (tests/CMakeLists.txt builds this once per
// LM_CF_CASE and requires the build to fail on the conversion / operation).
#include "lm/track_id.hpp"

#include <cstdint>

namespace {
void takesId(lm::TrackId) {}
} // namespace

void lm_compile_fail_case();
void lm_compile_fail_case() {
#if LM_CF_CASE == 1 // an integer (a row index, a queue position) is not a track id
    const int row = 3;
    takesId(row);
#elif LM_CF_CASE == 2 // nor does a track id turn back into an integer: use value()
    const std::int64_t n = lm::TrackId{7};
    (void)n;
#elif LM_CF_CASE == 3 // no arithmetic on ids
    const lm::TrackId next = lm::TrackId{7} + 1;
    (void)next;
#else
#error "LM_CF_CASE not set"
#endif
}
