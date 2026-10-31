// lm::net::RangeSet: merge, split, holes, serialisation.
#include "common.hpp"
#include "lm/net/range_set.hpp"

#include <random>
#include <vector>

using lm::net::RangeSet;
using R = RangeSet::Range;

int main() {
    {
        RangeSet s;
        s.add(10, 20);
        s.add(30, 40);
        CHECK((s.ranges() == std::vector<R>{{10, 20}, {30, 40}}));
        s.add(20, 30); // touching both: one range
        CHECK((s.ranges() == std::vector<R>{{10, 40}}));
        s.add(5, 12); // overlap at the front
        s.add(38, 50);
        CHECK((s.ranges() == std::vector<R>{{5, 50}}));
        s.add(7, 7); // empty
        s.add(9, 3); // reversed: ignored
        CHECK((s.ranges() == std::vector<R>{{5, 50}}));
        s.add(60, 70);
        s.add(0, 100); // swallows everything
        CHECK((s.ranges() == std::vector<R>{{0, 100}}));
    }
    {
        RangeSet s;
        s.add(0, 100);
        s.remove(40, 60); // split
        CHECK((s.ranges() == std::vector<R>{{0, 40}, {60, 100}}));
        s.remove(0, 10);
        s.remove(90, 200);
        CHECK((s.ranges() == std::vector<R>{{10, 40}, {60, 90}}));
        s.remove(30, 70); // across two ranges
        CHECK((s.ranges() == std::vector<R>{{10, 30}, {70, 90}}));
        CHECK_EQ(s.covered(), 40u);
    }
    {
        RangeSet s;
        s.add(10, 20);
        s.add(30, 40);
        CHECK(s.contains(10, 20));
        CHECK(s.contains(12, 18));
        CHECK(!s.contains(15, 25));
        CHECK(!s.contains(0, 1));
        CHECK(s.contains(5, 5)); // empty range
        CHECK_EQ(s.contiguousFrom(10), 20u);
        CHECK_EQ(s.contiguousFrom(19), 20u);
        CHECK_EQ(s.contiguousFrom(20), 20u);
        CHECK_EQ(s.contiguousFrom(0), 0u);
        CHECK((s.holes(50) == std::vector<R>{{0, 10}, {20, 30}, {40, 50}}));
        CHECK((s.holes(35) == std::vector<R>{{0, 10}, {20, 30}}));
        CHECK((s.holes(5) == std::vector<R>{{0, 5}}));
        CHECK((s.holeFrom(0, 50) == R{0, 10}));
        CHECK((s.holeFrom(12, 50) == R{20, 30}));
        CHECK((s.holeFrom(25, 50) == R{25, 30}));
        CHECK((s.holeFrom(35, 50) == R{40, 50}));
        CHECK(!s.holeFrom(35, 40));
        CHECK(!s.complete(40));
        RangeSet f;
        f.add(0, 40);
        CHECK(f.complete(40));
        CHECK(RangeSet().holes(0).empty());
    }
    {
        RangeSet s;
        s.add(0, 5);
        s.add(10, 1ull << 40);
        auto p = RangeSet::parse(s.serialize());
        CHECK(p && *p == s);
        CHECK(s.serialize() == "0-5,10-1099511627776");
        CHECK(RangeSet::parse("") && RangeSet::parse("")->empty());
        for (const char *bad : {"5", "5-", "-5", "5-5", "6-5", "0-5,5-9", "0-5,3-9", "0-5,", ",0-5", "0-5;9-10", "a-b",
                                "0-5 ", "99999999999999999999-1"})
            CHECK(!RangeSet::parse(bad));
    }
    {
        // Random operations against a bitmap model.
        std::mt19937 rng(1234);
        for (int round = 0; round < 200; round++) {
            RangeSet s;
            std::vector<bool> bits(300, false);
            for (int op = 0; op < 40; op++) {
                std::uint64_t a = rng() % 300, b = rng() % 300;
                if (a > b) std::swap(a, b);
                const bool add = rng() % 3 != 0;
                if (add) s.add(a, b);
                else s.remove(a, b);
                for (auto i = a; i < b; i++) bits[i] = add;
            }
            std::uint64_t cov = 0;
            for (bool x : bits) cov += x;
            CHECK_EQ(s.covered(), cov);
            // Disjoint, sorted, non-touching.
            for (std::size_t i = 1; i < s.ranges().size(); i++) CHECK(s.ranges()[i - 1].second < s.ranges()[i].first);
            std::uint64_t holeBytes = 0;
            for (auto [b, e] : s.holes(300)) {
                for (auto i = b; i < e; i++) CHECK(!bits[i]);
                holeBytes += e - b;
            }
            CHECK_EQ(holeBytes, 300 - cov);
            const std::uint64_t q = rng() % 300;
            std::uint64_t run = q;
            while (run < 300 && bits[run]) run++;
            CHECK_EQ(std::min<std::uint64_t>(s.contiguousFrom(q), 300), run);
            auto rt = RangeSet::parse(s.serialize());
            CHECK(rt && *rt == s);
        }
    }
    return lmtest::finish("test_range_set");
}
