// replay_bench - parse a LOBSTER message CSV and time the pure book replay.
// Usage: replay_bench <message_csv> [orderbook_csv]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lob/replay.hpp"

using namespace lob;

static Ts parse_ts(const char* s, const char** end) {
    // "34200.017459617" -> ns. Exact integer arithmetic, no doubles.
    Ts sec = 0; while (*s >= '0' && *s <= '9') sec = sec * 10 + (*s++ - '0');
    Ts frac = 0; int digits = 0;
    if (*s == '.') { ++s; while (*s >= '0' && *s <= '9') { if (digits < 9) { frac = frac * 10 + (*s - '0'); ++digits; } ++s; } }
    while (digits++ < 9) frac *= 10;
    *end = s;
    return sec * kNsPerSec + frac;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <message_csv>\n", argv[0]); return 1; }
    std::ifstream in(argv[1]);
    if (!in) { std::perror("open"); return 1; }
    std::vector<std::int64_t> ts, id, price; std::vector<std::int8_t> type, dir; std::vector<std::int32_t> size;
    std::string line;
    while (std::getline(in, line)) {
        const char* p = line.c_str(); const char* e;
        ts.push_back(parse_ts(p, &e)); p = e + 1;
        type.push_back(static_cast<std::int8_t>(std::strtol(p, const_cast<char**>(&e), 10))); p = e + 1;
        id.push_back(std::strtoll(p, const_cast<char**>(&e), 10)); p = e + 1;
        size.push_back(static_cast<std::int32_t>(std::strtol(p, const_cast<char**>(&e), 10))); p = e + 1;
        price.push_back(std::strtoll(p, const_cast<char**>(&e), 10)); p = e + 1;
        dir.push_back(static_cast<std::int8_t>(std::strtol(p, const_cast<char**>(&e), 10)));
    }
    MessageColumns cols{ts.data(), type.data(), id.data(), size.data(), price.data(), dir.data(), ts.size()};
    std::printf("loaded %zu messages\n", cols.n);

    const int reps = 5;
    double best_ms = 1e9;
    ReplayResult r;
    for (int k = 0; k < reps; ++k) {
        auto t0 = std::chrono::steady_clock::now();
        r = replay(cols, {}, ReplayOptions{});
        auto t1 = std::chrono::steady_clock::now();
        best_ms = std::min(best_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::printf("best of %d: %.2f ms  ->  %.1f M msgs/s  (%.0f ns/msg)\n", reps, best_ms,
                static_cast<double>(cols.n) / best_ms / 1e3, best_ms * 1e6 / static_cast<double>(cols.n));
    std::printf("unknown-id events: %llu, adds %llu, removes %llu, executes %llu\n",
                static_cast<unsigned long long>(r.apply_stats.unknown_id),
                static_cast<unsigned long long>(r.book_stats.adds),
                static_cast<unsigned long long>(r.book_stats.removes),
                static_cast<unsigned long long>(r.book_stats.executes));
    return 0;
}
