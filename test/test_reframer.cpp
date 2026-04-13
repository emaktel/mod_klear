// SPDX-License-Identifier: Apache-2.0
// Minimal unit tests for klear::Reframer. No framework — if something is
// wrong we abort() with a clear message.
#include "../src/reframer.h"

#include <cassert>
#include <cstdio>
#include <vector>

using klear::Reframer;

#define CHECK(cond) do {                                                      \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        return 1;                                                             \
    }                                                                         \
} while (0)

static int test_exact_multiple() {
    // 20 ms frames at 8 kHz: 160 samples in -> two 80-sample blocks.
    Reframer r(80);
    std::vector<int16_t> in(160);
    for (int i = 0; i < 160; ++i) in[i] = static_cast<int16_t>(i);

    r.push(in.data(), in.size());
    CHECK(r.has_block());

    int16_t out[80];
    CHECK(r.pop(out));
    for (int i = 0; i < 80; ++i) CHECK(out[i] == i);

    CHECK(r.has_block());
    CHECK(r.pop(out));
    for (int i = 0; i < 80; ++i) CHECK(out[i] == 80 + i);

    CHECK(!r.has_block());
    CHECK(r.pending() == 0);
    return 0;
}

static int test_non_multiple_buffers_remainder() {
    // Feed 90, pop one 80-block, expect 10 pending.
    Reframer r(80);
    std::vector<int16_t> in(90);
    for (int i = 0; i < 90; ++i) in[i] = static_cast<int16_t>(i);

    r.push(in.data(), in.size());
    int16_t out[80];
    CHECK(r.pop(out));
    CHECK(r.pending() == 10);
    CHECK(!r.has_block());

    // Feed another 70 → reaches 80 pending → one more block.
    std::vector<int16_t> more(70);
    for (int i = 0; i < 70; ++i) more[i] = static_cast<int16_t>(90 + i);
    r.push(more.data(), more.size());
    CHECK(r.has_block());
    CHECK(r.pop(out));
    for (int i = 0; i < 80; ++i) CHECK(out[i] == 80 + i);
    CHECK(r.pending() == 0);
    return 0;
}

static int test_wraparound() {
    // Capacity 4 blocks = 320 samples. Push 5 blocks over multiple calls,
    // popping between so the ring wraps.
    Reframer r(80, 320);
    int16_t out[80];
    int16_t sample = 0;

    auto push_n = [&](int n) {
        std::vector<int16_t> buf(n);
        for (int i = 0; i < n; ++i) buf[i] = sample++;
        r.push(buf.data(), n);
    };

    push_n(240);                 // 3 blocks pending
    CHECK(r.pop(out)); CHECK(r.pop(out));   // consumed 2, 1 remains
    push_n(240);                 // now 4 blocks pending, should wrap
    int popped = 0;
    while (r.has_block()) { CHECK(r.pop(out)); popped++; }
    CHECK(popped == 4);
    CHECK(r.pending() == 0);
    return 0;
}

static int test_grows_on_large_push() {
    Reframer r(80, 160);
    std::vector<int16_t> huge(4000);
    for (int i = 0; i < 4000; ++i) huge[i] = static_cast<int16_t>(i);
    r.push(huge.data(), huge.size());
    CHECK(r.pending() == 4000);

    int16_t out[80];
    int popped = 0;
    while (r.has_block()) { CHECK(r.pop(out)); popped++; }
    CHECK(popped == 50);  // 4000 / 80
    CHECK(r.pending() == 0);
    return 0;
}

int main() {
    int failures = 0;
    failures += test_exact_multiple();
    failures += test_non_multiple_buffers_remainder();
    failures += test_wraparound();
    failures += test_grows_on_large_push();
    if (failures == 0) {
        std::printf("reframer: all tests passed\n");
    } else {
        std::printf("reframer: %d failure(s)\n", failures);
    }
    return failures;
}
