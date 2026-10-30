// Checks dsp/hchip.cpp against the Rust emulator's output (expected/vectors.txt) sample
// for sample, then a few properties of the whole filter: the recipe words of
// the stock types, and the measured response of the 2-pole lowpass against the emulator's
// measured values.
// usage: hchip_test [expected/vectors.txt]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../dsp/emu_filters.hpp"
#include "../dsp/hchip.hpp"

static int fails = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fails++;                                           \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                 \
            std::fprintf(stderr, "\n");                        \
        }                                                      \
    } while (0)

static bool parseParams(const std::string& line, hchip::Params& p) {
    std::istringstream ss(line);
    std::string tok;
    ss >> tok;  // "params"
    while (ss >> tok) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos) return false;
        std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        int n = std::atoi(v.c_str());
        if (k == "acc_bits") p.accBits = n;
        else if (k == "acc_sat") p.accSaturate = n != 0;
        else if (k == "input_bits") p.inputBits = n;
        else if (k == "delay_bits") p.delayBits = n;
        else if (k == "mult_in_bits") p.multInBits = n;
        else if (k == "mult_in_sat") p.multInSaturate = n != 0;
        else if (k == "drop") p.multLsbDrop = n;
        else if (k == "mant_bits") p.mantBits = n;
        else if (k == "exact_sum") p.exactCoefSum = n != 0;
        else if (k == "round") p.productRound = v == "nearest" ? hchip::Rounding::Nearest : hchip::Rounding::Floor;
        else if (k == "product_sat") p.productSaturate = n != 0;
        else return false;
    }
    return true;
}

static int runVectors(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::string line;
    int cases = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("case ", 0) != 0) {
            std::fprintf(stderr, "unexpected line: %s\n", line.c_str());
            return 2;
        }
        std::string name = line.substr(5);
        hchip::Params p;
        std::getline(in, line);
        if (!parseParams(line, p)) {
            std::fprintf(stderr, "%s: bad params line\n", name.c_str());
            return 2;
        }
        uint32_t words[10];
        std::getline(in, line);
        {
            std::istringstream ss(line);
            std::string tok;
            ss >> tok;
            for (int i = 0; i < 10; i++) {
                ss >> tok;
                words[i] = static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 16));
            }
        }
        unsigned flags, x, n;
        std::getline(in, line);
        std::sscanf(line.c_str(), "flags %x", &flags);
        std::getline(in, line);
        std::sscanf(line.c_str(), "x %x", &x);
        std::getline(in, line);
        std::sscanf(line.c_str(), "n %u", &n);

        hchip::Voice v(p);
        v.setWords(words, static_cast<uint8_t>(flags));
        v.setX(static_cast<uint16_t>(x));
        int bad = 0;
        for (unsigned i = 0; i < n; i++) {
            std::getline(in, line);
            long long xin, want;
            std::sscanf(line.c_str(), "%lld %lld", &xin, &want);
            long long got = v.process(xin);
            if (got != want && bad++ < 3)
                std::fprintf(stderr, "FAIL %s sample %u: in %lld, got %lld, want %lld\n", name.c_str(), i, xin, got, want);
        }
        if (bad) fails++;
        std::getline(in, line);
        unsigned long long ds, ao, ms, ps;
        std::sscanf(line.c_str(), "stats %llu %llu %llu %llu", &ds, &ao, &ms, &ps);
        hchip::Stats st = v.stats();
        CHECK(st.delaySat == ds && st.accOverflow == ao && st.multInSat == ms && st.productSat == ps,
              "%s stats: got %llu %llu %llu %llu, want %llu %llu %llu %llu", name.c_str(),
              (unsigned long long)st.delaySat, (unsigned long long)st.accOverflow,
              (unsigned long long)st.multInSat, (unsigned long long)st.productSat, ds, ao, ms, ps);
        cases++;
        std::printf("%-32s %s\n", name.c_str(), bad ? "FAIL" : "ok");
    }
    std::printf("%d vector cases\n", cases);
    return 0;
}

// The recipes reproduce the words measured on the E4.
static void checkRecipes() {
    uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    emuz::Block b = emuz::computeFrames(0, 64, none);  // 2 Pole Low-pass, Q 64
    CHECK(b.w[0] == 0x6A07E6E3u && b.w[9] == 0x00F600FEu && b.flags == 0x40, "lp2 q64 words %08X %08X flags %02X", b.w[0], b.w[9], b.flags);
    b = emuz::computeFrames(0, 0, none);
    CHECK(b.w[0] == 0x8408FFCFu && b.w[9] == 0x00FE00FEu, "lp2 q0 words %08X %08X", b.w[0], b.w[9]);
    b = emuz::computeFrames(0, 127, none);
    CHECK(b.w[0] == 0x5205D4F3u && b.w[9] == 0x00EF00FEu, "lp2 q127 words %08X %08X", b.w[0], b.w[9]);
    b = emuz::computeFrames(2, 64, none);  // 6 Pole Low-pass, Q 64
    CHECK(b.w[0] == 0x7008ECDEu && b.w[4] == 0x7008ECDEu && b.w[8] == 0x7008ECDEu && b.w[9] == 0x00EC00FEu, "lp6 q64");
    b = emuz::computeFrames(4, 64, none);  // 4th Order High-pass, Q 64
    CHECK(b.w[0] == emuz::kNeutral && b.w[1] == 0x80048004u && b.w[5] == 0x80048004u && b.w[4] == 0x7004EFCFu &&
              b.w[8] == 0x7004EFCFu && b.w[9] == 0x00FE00FEu && b.flags == 0xC0,
          "hp4 q64");
    b = emuz::computeFrames(5, 64, none);  // 2nd Order Band-pass, Q 64: the words the reference vectors use
    CHECK(b.w[0] == 0xFF68FFCFu && b.w[1] == 0xFF68FFCFu && b.w[4] == emuz::kNeutral && b.w[5] == 0xFF04FF90u &&
              b.w[8] == 0x6808D8C8u && b.w[9] == 0x00FE00FEu && b.flags == 0xC0,
          "bp2 q64: %08X %08X %08X %08X %08X %08X", b.w[0], b.w[1], b.w[4], b.w[5], b.w[8], b.w[9]);
    b = emuz::computeFrames(15, 64, none);  // Vocal Ah-Ay-Ee, Q 64
    CHECK(b.w[0] == 0x70A570C1u && b.w[4] == 0x707F70ADu && b.w[8] == 0x70647045u && b.w[1] == 0xFFC0FFC0u &&
              b.w[9] == emuz::kNeutral && b.flags == 0x00,
          "vocal q64: %08X %08X %08X", b.w[0], b.w[4], b.w[8]);
}

// Steady-state gain of a sine through the voice.
static double measureDb(hchip::Voice& v, double f, double amp, double fs = 48000.0) {
    v.resetState();
    const double w = 2.0 * M_PI * f / fs;
    const int settle = 4096, n = 8192;
    double re = 0, im = 0;
    for (int i = 0; i < settle + n; i++) {
        long long x = llround(amp * std::sin(w * i));
        double y = double(v.process(x));
        if (i >= settle) {
            re += y * std::cos(-w * i);
            im += y * std::sin(-w * i);
        }
    }
    return 20.0 * std::log10(std::hypot(re, im) / (amp * n / 2.0));
}

// 2 Pole Lowpass, Q 64, cutoff 128: measured values (18-bit LATCH 18, input 9954).
static void checkLp2Response() {
    hchip::Params p;
    p.multInBits = 18;
    hchip::Voice v(p);
    uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    emuz::Block b = emuz::computeFrames(0, 64, none);
    v.setWords(b.w, b.flags);
    v.setX(hchip::dup(128));
    const double want[][2] = {{100, -0.66}, {500, 1.00}, {1000, 7.75}, {2000, -7.22}, {5000, -25.55}, {10000, -36.97}};
    for (auto& r : want) {
        double g = measureDb(v, r[0], 9954.0);
        CHECK(std::fabs(g - r[1]) < 0.05, "lp2 q64 c128 at %g Hz: %.2f dB, want %.2f", r[0], g, r[1]);
    }
    // 6 Pole Lowpass, Q 64, cutoff 128, input 2340
    b = emuz::computeFrames(2, 64, none);
    v.setWords(b.w, b.flags);
    const double want6[][2] = {{200, -0.70}, {500, 3.82}, {1000, 20.31}, {2000, -24.74}};
    for (auto& r : want6) {
        double g = measureDb(v, r[0], 2340.0);
        CHECK(std::fabs(g - r[1]) < 0.05, "lp6 q64 c128 at %g Hz: %.2f dB, want %.2f", r[0], g, r[1]);
    }
    // 4th Order Highpass, Q 64, cutoff 128
    b = emuz::computeFrames(4, 64, none);
    v.setWords(b.w, b.flags);
    // Below 1 kHz the signal between the zero section and the pole section is a few hundred LSB
    // and the measurement is noise-limited (tests/voice.rs allows 0.5 dB at 300 Hz).
    const double want4[][3] = {{500, -15.05, 0.3}, {1000, 10.52, 0.05}, {2000, 3.37, 0.05}, {5000, 0.86, 0.05}};
    for (auto& r : want4) {
        double g = measureDb(v, r[0], 9000.0);
        CHECK(std::fabs(g - r[1]) < r[2], "hp4 q64 c128 at %g Hz: %.2f dB, want %.2f", r[0], g, r[1]);
    }
}

// The decoder tables of coef.rs.
static void checkDecoder() {
    CHECK(hchip::decodeLinear(0) == 1, "table(0)");
    CHECK(hchip::decodeLinear(hchip::kCodeOneF) == (1 << 16), "table(CF)");
    CHECK(hchip::decodeLinear(hchip::kCodeOneX) == (1 << 18), "table(FF)");
    int64_t prev = -1;
    for (int c = 0; c < 256; c++) {
        int64_t v = hchip::decodeLinear(uint8_t(c));
        CHECK(v > prev, "table monotonic at %d", c);
        CHECK(hchip::decodeQ8(uint16_t(c << 8)) == v, "q8 at %d", c);
        prev = v;
    }
    // word 50405040: B2' = 0.001038, f^2 = 0.002075 (VOICE_INTERFACE.md)
    hchip::Decoder d;
    double b2 = d.decode(0x50 << 8, hchip::Role::Second).toDouble();
    double b1 = d.decode(0x40 << 8, hchip::Role::First).toDouble();
    CHECK(std::fabs(b2 - 0.001038) < 1e-6, "B2' %f", b2);
    CHECK(std::fabs(b1 - b2 - 0.002075) < 1e-6, "f2 %f", b1 - b2);
    CHECK(d.decode(0xFF << 8, hchip::Role::Second).toDouble() == 1.0, "neutral X");
    CHECK(d.decode(0xCF << 8, hchip::Role::First).toDouble() == 2.0, "neutral F");
    // interpolation: cutoff 64 of word 6A07E6E3 gives X 137.12, F 62.22
    uint8_t x0 = 0x6A, f0 = 0x07, x255 = 0xE6, f255 = 0xE3;
    CHECK(std::fabs(hchip::interpX(x0, x255, hchip::dup(64)) / 256.0 - 137.12) < 0.01, "X at 64");
    CHECK(std::fabs(hchip::interpX(f0, f255, hchip::dup(64)) / 256.0 - 62.22) < 0.01, "F at 64");
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "expected/vectors.txt";
    checkDecoder();
    checkRecipes();
    int rc = runVectors(path);
    if (rc) return rc;
    checkLp2Response();
    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
