// Compile-fail cases for interface types and the server JSON writer (see
// tests/CMakeLists.txt).
#include "lm/decoder.hpp"
#include "lm/source.hpp"
#if LM_CF_CASE >= 3
#include "json.hpp"
#include <cstddef>
#endif

namespace {
// Minimal implementations: copying one must fail because the interface deletes copying
// (an implementation would otherwise get an implicit copy that duplicates owned state).
struct Src final : lm::ByteSource {
    std::size_t read(void *, std::size_t) override { return 0; }
    bool seek(std::int64_t, Whence) override { return false; }
    std::int64_t tell() const override { return 0; }
    std::int64_t size() const override { return -1; }
    const std::string &name() const override { return name_; }
    std::string name_;
};
struct Dec final : lm::Decoder {
    lm::Format format() const override { return {}; }
    std::uint32_t sampleRate() const override { return 0; }
    std::uint32_t channels() const override { return 0; }
    lm::Frames totalFrames() const override { return {}; }
    lm::Frames read(float *, lm::Frames) override { return {}; }
    bool seek(lm::Frames) override { return false; }
};
} // namespace

void lm_compile_fail_case(Src &s, Dec &d);
void lm_compile_fail_case(Src &s, Dec &d) {
#if LM_CF_CASE == 1 // interfaces are owned through unique_ptr, never copied
    Src copy = s;
    (void)copy;
    (void)d;
#elif LM_CF_CASE == 2
    Dec copy = d;
    (void)copy;
    (void)s;
#elif LM_CF_CASE == 3 // size_t has no exact Json::value overload
    (void)s;
    (void)d;
    lm::server::Json j;
    j.kv("n", std::size_t{1});
#else
#error "LM_CF_CASE not set"
#endif
}
