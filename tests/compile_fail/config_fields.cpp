// Compile-fail cases for the libconfig field builders (tests/CMakeLists.txt builds this
// once per LM_CF_CASE and requires the build to fail naming the violated concept).
#include "lm/config/config.hpp"

namespace {
enum class Mode { A, B };
struct Cfg {
    bool flag = false;
    double gain = 0.0;
    int n = 0;
    Mode mode = Mode::A;
};
} // namespace

void lm_compile_fail_case();
void lm_compile_fail_case() {
#if LM_CF_CASE == 1 // a bool member is not a number: use boolField
    (void)lm::config::intField<Cfg>("flag", &Cfg::flag, 0, 1);
#elif LM_CF_CASE == 2 // a floating-point member: use floatField
    (void)lm::config::intField<Cfg>("gain", &Cfg::gain, 0, 1);
#elif LM_CF_CASE == 3 // an int member is not an enum
    (void)lm::config::enumField<Cfg>("n", &Cfg::n, std::vector<std::pair<std::string, int>>{{"x", 1}});
#else
#error "LM_CF_CASE not set"
#endif
}
