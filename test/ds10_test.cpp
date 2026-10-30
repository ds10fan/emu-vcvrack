// The DS10 drive formula: identity at drive 0, hard clip of 4x the input at drive 1, a linear
// crossfade in between.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../dsp/ds10_drive.hpp"

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            fails++;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    };
    for (float x : {-1.f, -0.3f, 0.f, 0.1f, 0.7f, 1.f}) {
        check(ds10::oneSample(x, 0.f) == x, "drive 0 is the identity");
        float want1 = x * 4.f < -1.f ? -1.f : (x * 4.f > 1.f ? 1.f : x * 4.f);
        check(ds10::oneSample(x, 1.f) == want1, "drive 1 is clamp(4x)");
        float d = 0.5f;
        float clipped = x * 2.f < -1.f ? -1.f : (x * 2.f > 1.f ? 1.f : x * 2.f);
        check(std::fabs(ds10::oneSample(x, d) - (0.5f * x + 0.5f * clipped)) < 1e-6f, "drive 0.5 is the half mix");
    }
    check(ds10::oneSample(0.1f, 0.25f) == 0.1f, "drive 0.25 on 0.1: 4 * 0.25 * 0.1 = 0.1, mixed with 0.1");
    std::printf(fails ? "%d FAILURE(S)\n" : "ds10 test passed\n", fails);
    return fails ? 1 : 0;
}
