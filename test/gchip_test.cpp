// dsp/gchip.cpp against the test vectors and invariants of gchip_interpolator_spec.md (sections
// 4, 7 and 14), the fixed-point path against the exact one, the playback voice's looping, and
// dsp/wav.cpp on files written here.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "../dsp/gchip.hpp"
#include "../dsp/wav.hpp"

static int fails = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fails++;                                                  \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                        \
            std::fprintf(stderr, "\n");                               \
        }                                                             \
    } while (0)

static void checkTable() {
    double sum = 0, mx = -1, mn = 1;
    int imx = -1, imn = -1;
    for (int i = 0; i < 223; i++) {
        double v = gchip::proto(i);
        sum += v;
        if (v > mx) { mx = v; imx = i; }
        if (v < mn) { mn = v; imn = i; }
    }
    CHECK(std::fabs(sum - 1.055788510) < 1e-9, "sum %.9f", sum);
    CHECK(imx == 111 && std::fabs(mx - 3.410910e-02) < 1e-9, "max %g at %d", mx, imx);
    CHECK(imn == 70 && std::fabs(mn + 5.938340e-03) < 1e-9, "min %g at %d", mn, imn);
    for (int a = 0; a <= 111; a++) CHECK(gchip::proto(111 - a) == gchip::proto(111 + a), "symmetry at %d", a);
    CHECK(std::fabs(gchip::norm() - 30.309100442) < 1e-8, "norm %.9f", gchip::norm());
    CHECK(gchip::rom(112) == 0.0 && gchip::romFixed(112) == 0, "address 112 reads zero");
}

static void checkCoefficientVectors() {
    struct V {
        int fMs;
        double fLs;
        double c[7];
    };
    const V vectors[] = {
        {0, 0.0, {+0.060867645, -0.138883269, +0.578106689, +0.578106689, -0.138883269, +0.060867645, +0.000000000}},
        {1, 0.0, {+0.057447263, -0.127631016, +0.530230434, +0.625452535, -0.150280703, +0.069924004, -0.005349011}},
        {8, 0.0, {+0.032580768, -0.037982152, +0.203098554, +0.904435681, -0.178371481, +0.075670003, +0.000578276}},
        {12, 0.8, {+0.017878465, +0.017530262, +0.019066073, +1.012038443, -0.135759250, +0.064106960, +0.005104719}},
        {16, 0.0, {+0.010430816, +0.045409094, -0.072771241, +1.033816138, -0.072771241, +0.045409094, +0.010430816}},
        {24, 0.0, {+0.000578276, +0.075670003, -0.178371481, +0.904435681, +0.203098554, -0.037982152, +0.032580768}},
        {31, 0.0, {-0.005349011, +0.069924004, -0.150280703, +0.625452535, +0.530230434, -0.127631016, +0.057447263}},
    };
    for (const V& v : vectors) {
        double c[7];
        gchip::coeffs(v.fMs, v.fLs, c);
        for (int n = 0; n < 7; n++)
            CHECK(std::fabs(c[n] - v.c[n]) < 1e-9, "f_MS %d f_LS %.1f tap %d: %.9f want %.9f", v.fMs, v.fLs, n, c[n], v.c[n]);
    }
    // Invariants: sum 1 +- 0.0002 at every phase, symmetry at f = 0 and 0.5, DC ripple
    double gmin = 1e9, gmax = -1e9;
    for (int k = 0; k < 4096; k++) {
        double f = k / 4096.0;
        double xf = f * 32;
        int fMs = int(std::floor(xf));
        double c[7];
        gchip::coeffs(fMs, xf - fMs, c);
        double s = 0;
        for (double x : c) s += x;
        // The spec says +-0.0002; its own vector at f_MS = 1 sums to 0.999793506, so 0.00025.
        CHECK(std::fabs(s - 1.0) < 0.00025, "sum at phase %d: %.6f", k, s);
        if (s < gmin) gmin = s;
        if (s > gmax) gmax = s;
    }
    double ripple = 20 * std::log10(gmax / gmin);
    CHECK(ripple <= 0.0034 + 1e-4, "DC ripple %.4f dB", ripple);
    double c[7];
    gchip::coeffs(0, 0.0, c);
    CHECK(c[6] == 0.0 && c[0] == c[5] && c[1] == c[4] && c[2] == c[3], "f = 0 symmetric with tap 6 zero");
    gchip::coeffs(16, 0.0, c);
    CHECK(c[0] == c[6] && c[1] == c[5] && c[2] == c[4] && c[3] > 1.0, "f = 0.5 symmetric about tap 3");
}

static void checkResampleVector() {
    std::vector<double> x(4096);
    for (size_t n = 0; n < x.size(); n++) x[n] = std::sin(2 * M_PI * 0.05 * n + 0.37);
    std::vector<double> y;
    gchip::resampleReference(x, std::pow(2.0, -1.0 / 3.0), 12, y);
    const double want[12] = {-0.207208627, -0.437475860, -0.640706795, -0.804246856, -0.917795743, -0.975552895,
                             -0.971572789, -0.908353529, -0.788697463, -0.620221823, -0.413390344, -0.181258897};
    for (int n = 0; n < 12; n++) CHECK(std::fabs(y[n] - want[n]) < 1e-9, "y[%d] = %.9f want %.9f", n, y[n], want[n]);
}

// Alias floor: a sine at 0.1 fs shifted up by 2^(1/3) through the Voice; the worst spur relative
// to the fundamental must be far below the -43 dB of a blend-less implementation (spec section 14).
static double aliasFloorDb(bool fixedPoint) {
    gchip::Sample s;
    s.data.resize(200000);
    const double f0 = 0.1;
    for (size_t n = 0; n < s.data.size(); n++) s.data[n] = int16_t(std::lround(30000.0 * std::sin(2 * M_PI * f0 * n)));
    gchip::Voice v;
    v.trigger(&s, 100, 0, 0, gchip::LoopMode::Off);
    const double ratio = std::pow(2.0, 1.0 / 3.0);
    v.setIncrement(uint64_t(std::llround(ratio * 4294967296.0)));
    const int N = 16384;
    std::vector<double> y(N);
    bool ended;
    for (int n = 0; n < 1000; n++) v.process(fixedPoint, ended);
    for (int n = 0; n < N; n++) y[n] = v.process(fixedPoint, ended) / 32768.0;
    // Blackman-Harris windowed DFT; the fundamental is at f0 * ratio of the output rate.
    const double fout = f0 * ratio;
    auto mag = [&](double f) {
        double re = 0, im = 0;
        for (int n = 0; n < N; n++) {
            double t = 2 * M_PI * n / (N - 1);
            double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
            re += w * y[n] * std::cos(2 * M_PI * f * n);
            im -= w * y[n] * std::sin(2 * M_PI * f * n);
        }
        return std::hypot(re, im);
    };
    double fund = mag(fout);
    double worst = 0;
    for (int k = 1; k < N / 2; k += 2) {
        double f = double(k) / N;
        if (std::fabs(f - fout) < 8.0 / N) continue;  // the main lobe
        double m = mag(f);
        if (m > worst) worst = m;
    }
    return 20 * std::log10(worst / fund);
}

static void checkVoice() {
    // Exact and fixed-point paths agree closely on a sine at ratio 1.
    gchip::Sample s;
    s.data.resize(5000);
    for (size_t n = 0; n < s.data.size(); n++) s.data[n] = int16_t(std::lround(20000.0 * std::sin(2 * M_PI * 0.013 * n)));
    gchip::Voice a, b;
    a.trigger(&s, 0, 0, 0, gchip::LoopMode::Off);
    b.trigger(&s, 0, 0, 0, gchip::LoopMode::Off);
    uint64_t inc = uint64_t(std::llround(1.2345 * 4294967296.0));
    a.setIncrement(inc);
    b.setIncrement(inc);
    double maxDiff = 0;
    bool ea, eb;
    for (int n = 0; n < 3000; n++) {
        double ya = a.process(false, ea), yb = b.process(true, eb);
        maxDiff = std::max(maxDiff, std::fabs(ya - yb));
    }
    // 16-bit coefficients at unity 2^14 and an 8-bit blend: a few LSB on a 20000 LSB sine.
    CHECK(maxDiff < 16.0, "fixed vs exact: max difference %.2f LSB", maxDiff);

    // At f = 0 the taps sit at +-0.5, +-1.5, +-2.5, +3.5 samples about i + 0.5 (spec section 5), so
    // the output point is x(i + f + 0.5): playing at ratio 1 from the start reproduces the sine
    // half a sample ahead (the spec's resampling vector confirms this: y[0] = x(9.5) filtered).
    gchip::Voice c;
    c.trigger(&s, 0, 0, 0, gchip::LoopMode::Off);
    c.setIncrement(uint64_t(1) << 32);
    double err = 0;
    bool ec;
    for (int n = 0; n < 4000; n++) {
        double y = c.process(false, ec);
        if (n >= 10) {
            double want = 20000.0 * std::sin(2 * M_PI * 0.013 * (n + 0.5));
            err = std::max(err, std::fabs(y - want));
        }
    }
    CHECK(err < 20000.0 * 0.01, "ratio 1 playback error %.1f LSB", err);

    // One-shot ends exactly once; looping On never ends; Gate release plays through to the end.
    gchip::Voice d;
    d.trigger(&s, 4990, 0, 0, gchip::LoopMode::Off);
    d.setIncrement(uint64_t(1) << 32);
    int ends = 0;
    for (int n = 0; n < 100; n++) {
        bool e;
        d.process(false, e);
        ends += e;
    }
    CHECK(ends == 1 && !d.active(), "one-shot ended %d times", ends);

    gchip::Voice l;
    l.trigger(&s, 0, 1000, 1100, gchip::LoopMode::On);
    l.setIncrement(uint64_t(1) << 32);
    for (int n = 0; n < 20000; n++) {
        bool e;
        l.process(false, e);
        CHECK(!e, "looping voice ended at %d", n);
    }
    CHECK(l.active() && l.position() >= 1000 && l.position() < 1100, "looping voice inside the loop (%lld)", (long long)l.position());

    gchip::Voice g;
    g.trigger(&s, 0, 1000, 1100, gchip::LoopMode::Gate);
    g.setIncrement(uint64_t(1) << 32);
    for (int n = 0; n < 3000; n++) {
        bool e;
        g.process(false, e);
    }
    CHECK(g.position() >= 1000 && g.position() < 1100, "gated voice loops while held");
    g.release();
    int n = 0;
    bool e = false;
    while (g.active() && n < 10000) {
        g.process(false, e);
        n++;
    }
    CHECK(e && !g.active() && n > 3800 && n < 4200, "released voice played to the end in %d samples", n);

    // A loop over a whole number of periods of the sine is seamless: the output stays a clean
    // sine across the wrap (the wrapped reads stand in for guard samples).
    gchip::Sample p;
    p.data.resize(4000);
    const double fp = 1.0 / 50.0;  // period 50 samples
    for (size_t k = 0; k < p.data.size(); k++) p.data[k] = int16_t(std::lround(20000.0 * std::sin(2 * M_PI * fp * k)));
    gchip::Voice w;
    w.trigger(&p, 0, 1000, 1500, gchip::LoopMode::On);  // 10 periods
    w.setIncrement(uint64_t(std::llround(0.9 * 4294967296.0)));
    double werr = 0;
    double pos = 0;
    for (int k = 0; k < 30000; k++) {
        bool ee;
        double y = w.process(false, ee);
        if (k > 100) {
            double want = 20000.0 * std::sin(2 * M_PI * fp * (pos + 0.5));
            werr = std::max(werr, std::fabs(y - want));
        }
        pos += 0.9;
    }
    CHECK(werr < 20000.0 * 0.02, "seamless loop error %.1f LSB", werr);
}

// Write a WAV in memory: PCM or float, with an optional smpl loop.
static std::vector<uint8_t> makeWav(int channels, int bits, bool isFloat, double rate, const std::vector<double>& frames,
                                    bool withLoop, uint32_t loopStart, uint32_t loopEndIncl) {
    std::vector<uint8_t> b;
    auto put16 = [&](uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 255); };
    auto tag = [&](const char* t) { b.insert(b.end(), t, t + 4); };
    tag("RIFF"); put32(0); tag("WAVE");
    tag("fmt "); put32(16); put16(isFloat ? 3 : 1); put16(channels); put32(uint32_t(rate));
    put32(uint32_t(rate) * channels * bits / 8); put16(channels * bits / 8); put16(bits);
    if (withLoop) {
        tag("smpl"); put32(36 + 24);
        for (int i = 0; i < 7; i++) put32(0);
        put32(1); put32(0);
        put32(0); put32(0); put32(loopStart); put32(loopEndIncl); put32(0); put32(0);
    }
    tag("data"); put32(uint32_t(frames.size() * bits / 8));
    for (double v : frames) {
        if (isFloat) {
            float f = float(v);
            uint32_t u;
            std::memcpy(&u, &f, 4);
            put32(u);
        } else if (bits == 16) {
            put16(uint16_t(int16_t(std::lround(v * 32767))));
        } else if (bits == 24) {
            int32_t s = int32_t(std::lround(v * 8388607));
            b.push_back(s & 255); b.push_back((s >> 8) & 255); b.push_back((s >> 16) & 255);
        } else if (bits == 8) {
            b.push_back(uint8_t(std::lround(v * 127) + 128));
        }
    }
    uint32_t riffLen = uint32_t(b.size() - 8);
    for (int i = 0; i < 4; i++) b[4 + i] = (riffLen >> (8 * i)) & 255;
    return b;
}

static void checkWav() {
    std::vector<double> frames;
    for (int n = 0; n < 100; n++) frames.push_back(0.5 * std::sin(0.1 * n));
    wav::File f;
    std::string err;
    auto bytes = makeWav(1, 16, false, 44100, frames, true, 10, 59);
    CHECK(wav::parse(bytes.data(), bytes.size(), f, err), "16-bit mono: %s", err.c_str());
    CHECK(f.mono.size() == 100 && f.rate == 44100 && f.channels == 1 && f.bitsPerSample == 16, "16-bit mono fields");
    CHECK(f.hasLoop && f.loopStart == 10 && f.loopEnd == 60, "loop %lld..%lld", (long long)f.loopStart, (long long)f.loopEnd);
    double maxErr = 0;
    for (int n = 0; n < 100; n++) maxErr = std::max(maxErr, std::fabs(f.mono[n] / 32768.0 - frames[n]));
    CHECK(maxErr < 1.0 / 32768 + 1e-6, "16-bit values (%g)", maxErr);

    // stereo 24-bit: L = x, R = -x averages to zero; L = R = x comes back as x
    std::vector<double> st;
    for (int n = 0; n < 50; n++) { st.push_back(frames[n]); st.push_back(frames[n]); }
    bytes = makeWav(2, 24, false, 48000, st, false, 0, 0);
    CHECK(wav::parse(bytes.data(), bytes.size(), f, err), "24-bit stereo: %s", err.c_str());
    CHECK(f.mono.size() == 50 && f.channels == 2 && !f.hasLoop, "24-bit stereo fields");
    maxErr = 0;
    for (int n = 0; n < 50; n++) maxErr = std::max(maxErr, std::fabs(f.mono[n] / 32768.0 - frames[n]));
    CHECK(maxErr < 1.0 / 32768 + 1e-6, "24-bit values (%g)", maxErr);

    bytes = makeWav(1, 32, true, 96000, frames, false, 0, 0);
    CHECK(wav::parse(bytes.data(), bytes.size(), f, err), "float: %s", err.c_str());
    CHECK(f.isFloat && f.rate == 96000, "float fields");
    maxErr = 0;
    for (int n = 0; n < 100; n++) maxErr = std::max(maxErr, std::fabs(f.mono[n] / 32768.0 - frames[n]));
    CHECK(maxErr < 1.0 / 32768 + 1e-6, "float values (%g)", maxErr);

    bytes = makeWav(1, 8, false, 8000, frames, false, 0, 0);
    CHECK(wav::parse(bytes.data(), bytes.size(), f, err), "8-bit: %s", err.c_str());
    maxErr = 0;
    for (int n = 0; n < 100; n++) maxErr = std::max(maxErr, std::fabs(f.mono[n] / 32768.0 - frames[n]));
    CHECK(maxErr < 1.0 / 127 + 1e-6, "8-bit values (%g)", maxErr);

    const char junk[] = "RIFF\0\0\0\0WAVEjunk";
    CHECK(!wav::parse(reinterpret_cast<const uint8_t*>(junk), sizeof junk - 1, f, err), "junk rejected");
    CHECK(!wav::load("/nonexistent/file.wav", f, err) && err == "cannot open file", "missing file: %s", err.c_str());
}

int main() {
    checkTable();
    checkCoefficientVectors();
    checkResampleVector();
    checkVoice();
    checkWav();
    double exact = aliasFloorDb(false), fixed = aliasFloorDb(true);
    std::printf("alias floor, sine 0.1 fs up 2^(1/3): exact %.1f dB, fixed-point %.1f dB\n", exact, fixed);
    CHECK(exact < -60.0, "exact alias floor %.1f dB", exact);
    CHECK(fixed < -60.0, "fixed-point alias floor %.1f dB", fixed);
    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::printf("gchip test passed\n");
    return 0;
}
