// Range header parsing for /stream (decisions documented in server/src/range.hpp).
#include "common.hpp"
#include "range.hpp"

using lm::server::ByteRange;
using lm::server::parse_range;
using K = ByteRange::Kind;

static void expect(const char *h, std::uint64_t size, K kind, std::uint64_t first = 0,
                   std::uint64_t last = 0, int line = 0) {
    ByteRange r = parse_range(h, size);
    bool ok = r.kind == kind && (kind != K::Satisfiable || (r.first == first && r.last == last));
    if (!ok) {
        std::fprintf(stderr, "line %d: '%s' size %llu -> kind %d %llu-%llu\n", line, h,
                     (unsigned long long)size, int(r.kind), (unsigned long long)r.first,
                     (unsigned long long)r.last);
        ++lmtest::g_failures;
    }
}
#define EXPECT(...) expect(__VA_ARGS__, __LINE__)

int main() {
    // absent / other units → no range (full 200)
    EXPECT("", 100, K::None, 0, 0);
    EXPECT("items=0-5", 100, K::None, 0, 0);
    // closed
    EXPECT("bytes=0-0", 100, K::Satisfiable, 0, 0);
    EXPECT("bytes=0-99", 100, K::Satisfiable, 0, 99);
    EXPECT("bytes=10-20", 100, K::Satisfiable, 10, 20);
    EXPECT("bytes= 10 - 20 ", 100, K::Satisfiable, 10, 20);
    // last beyond EOF is clamped
    EXPECT("bytes=50-1000", 100, K::Satisfiable, 50, 99);
    // open-ended
    EXPECT("bytes=0-", 100, K::Satisfiable, 0, 99);
    EXPECT("bytes=99-", 100, K::Satisfiable, 99, 99);
    // suffix
    EXPECT("bytes=-1", 100, K::Satisfiable, 99, 99);
    EXPECT("bytes=-30", 100, K::Satisfiable, 70, 99);
    EXPECT("bytes=-100", 100, K::Satisfiable, 0, 99);
    EXPECT("bytes=-500", 100, K::Satisfiable, 0, 99);  // longer than file → whole file
    EXPECT("bytes=-0", 100, K::Unsatisfiable, 0, 0);
    // start past EOF
    EXPECT("bytes=100-", 100, K::Unsatisfiable, 0, 0);
    EXPECT("bytes=100-200", 100, K::Unsatisfiable, 0, 0);
    EXPECT("bytes=1000-2000", 100, K::Unsatisfiable, 0, 0);
    // empty file: nothing is satisfiable
    EXPECT("bytes=0-", 0, K::Unsatisfiable, 0, 0);
    EXPECT("bytes=-5", 0, K::Unsatisfiable, 0, 0);
    // malformed
    EXPECT("bytes=20-10", 100, K::Invalid, 0, 0);  // reversed
    EXPECT("bytes=-", 100, K::Invalid, 0, 0);
    EXPECT("bytes=", 100, K::Invalid, 0, 0);
    EXPECT("bytes=a-b", 100, K::Invalid, 0, 0);
    EXPECT("bytes=1-2-3", 100, K::Invalid, 0, 0);
    EXPECT("bytes=+1-2", 100, K::Invalid, 0, 0);
    EXPECT("bytes 0-1", 100, K::Invalid, 0, 0);
    EXPECT("bytes=99999999999999999999999-", 100, K::Invalid, 0, 0);  // overflow
    // multiple ranges: only the first is used; all must be well-formed
    EXPECT("bytes=0-9,20-29", 100, K::Satisfiable, 0, 9);
    EXPECT("bytes=-10, 0-5", 100, K::Satisfiable, 90, 99);
    EXPECT("bytes=200-300,0-5", 100, K::Unsatisfiable, 0, 0);
    EXPECT("bytes=0-9,x", 100, K::Invalid, 0, 0);
    EXPECT("bytes=0-9,", 100, K::Invalid, 0, 0);
    // large files
    EXPECT("bytes=4294967296-", 8589934592ULL, K::Satisfiable, 4294967296ULL, 8589934591ULL);
    CHECK_EQ(parse_range("bytes=10-19", 100).length(), 10u);
    return lmtest::finish("test_server_range");
}
