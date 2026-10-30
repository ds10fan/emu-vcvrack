// dsp/midiverb.cpp: the ROMs, program tables and properties that need no reference, and, when
// test/expected/midiverb.txt exists (written by test/midiverb_ref.js from the original
// JavaScript), the same cases compared bit for bit.
// usage: midiverb_test [expected/midiverb.txt]      midiverb_test --print   (this port's output, same format)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../dsp/midiverb.hpp"

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

// ---- the cases of test/midiverb_ref.js ----
static const int N = 24000;

struct Change {
    int at, rom, program;  // rom >= 0: a ROM change (program goes to the ROM's first)
};
struct Case {
    const char* name;
    int rom, program;
    double mix, fb;
    std::vector<Change> changes;
};

static std::vector<Case> cases() {
    return {
        {"mv2_p1", 2, 1, 0.5, 0, {}},
        {"mv2_p29_fb", 2, 29, 1, 0.3, {}},
        {"mv2_p51_lfo_tri", 2, 51, 0.8, 0, {}},
        {"mv2_p57_lfo_tri", 2, 57, 1, 0.2, {}},
        {"mv2_p60_lfo_sine", 2, 60, 1, 0, {}},
        {"mv2_p68_lfo_sine", 2, 68, 0.7, 0.1, {}},
        {"mv2_p97_regen", 2, 97, 1, 0.9, {}},
        {"mv1_p1", 0, 1, 0.5, 0, {}},
        {"mv1_p50_fb", 0, 50, 1, 1, {}},
        {"mv1_p61_reverse", 0, 61, 1, 0, {}},
        {"mfx_p37_regen", 1, 37, 1, 0.5, {}},
        {"mfx_p58_thick", 1, 58, 0.6, 0, {}},
        {"changes", 2, 5, 1, 0.25, {{4096, -1, 53}, {12288, 0, 0}, {16384, -1, 20}}},
    };
}

static void input(int i, uint32_t& s, double& l, double& r) {
    l = r = 0;
    if (i == 0) {
        l = 0.5;
        r = 0.25;
    } else if (i >= 1000 && i < 3000) {
        s = s * 1664525u + 1013904223u;
        l = (double(s >> 8) / 16777216 * 2 - 1) * 0.5;
        s = s * 1664525u + 1013904223u;
        r = (double(s >> 8) / 16777216 * 2 - 1) * 0.5;
    } else if (i >= 6000 && i < 8000) {
        l = ((i / 50) % 2) ? 0.3 : -0.3;
        r = -l;
    }
}

static uint64_t fnv(uint64_t h, double v) {
    unsigned char b[8];
    std::memcpy(b, &v, 8);
    for (unsigned char x : b) {
        h ^= x;
        h *= 0x100000001b3ull;
    }
    return h;
}

struct Result {
    std::vector<std::string> lines;  // "i l r"
    std::vector<double> l, r;        // the same samples
    uint64_t hash = 0;
    double peakTail = 0;             // largest |out| over the last 1000 samples
    double peak = 0;
    bool finite = true;
};

static Result run(const Case& c) {
    midiverb::Emulator e;
    e.selectRom(c.rom);
    e.selectProgram(c.program);
    e.dryWetMix = c.mix;
    e.feedbackAmount = c.fb;
    Result res;
    res.hash = 0xcbf29ce484222325ull;
    uint32_t s = 12345;
    for (int i = 0; i < N; i++) {
        for (const Change& ch : c.changes) {
            if (ch.at != i) continue;
            if (ch.rom >= 0) e.selectRom(ch.rom);
            else e.selectProgram(ch.program);
        }
        double l, r, ol, orr;
        input(i, s, l, r);
        e.process(l, r, ol, orr);
        res.hash = fnv(fnv(res.hash, ol), orr);
        if (!std::isfinite(ol) || !std::isfinite(orr)) res.finite = false;
        res.peak = std::max(res.peak, std::max(std::fabs(ol), std::fabs(orr)));
        if (i >= N - 1000) res.peakTail = std::max(res.peakTail, std::max(std::fabs(ol), std::fabs(orr)));
        if (i < 32 || (i >= 2000 && i < 2016)) {
            res.l.push_back(ol);
            res.r.push_back(orr);
            char buf[96];
            std::snprintf(buf, sizeof buf, "%d %.17g %.17g", i, ol, orr);
            res.lines.push_back(buf);
        }
    }
    return res;
}

// ---- checks without a reference ----
static void checkTables() {
    CHECK(midiverb::romCount() == 3, "three ROMs");
    const int sizes[3] = {32768, 16384, 32768};
    const int counts[3] = {64, 64, 100};
    for (int k = 0; k < 3; k++) {
        const midiverb::RomInfo& r = midiverb::rom(k);
        CHECK(int(r.size) == sizes[k], "%s size %zu", r.name, r.size);
        CHECK(r.programCount() == counts[k], "%s programs %d", r.name, r.programCount());
        // every program's 256 bytes lie inside the image
        CHECK(r.bytecodeOffset + r.programCount() * 256 <= int(r.size), "%s programs fit", r.name);
        for (int p = 0; p < r.programCount(); p++) CHECK(r.effectNames[p] && r.effectNames[p][0], "%s name %d", r.name, p);
    }
    CHECK(std::strcmp(midiverb::rom(0).effectNames[63], "DEFEAT") == 0, "MIDIVerb 64 is DEFEAT");
    CHECK(std::strcmp(midiverb::rom(2).effectNames[0], "Defeat") == 0, "MIDIVerb 2 0 is Defeat");
    CHECK(midiverb::lfoPatch(49) == nullptr && midiverb::lfoPatch(50) && midiverb::lfoPatch(69) && !midiverb::lfoPatch(70), "LFO programs 50..69");
}

// Every program of every ROM: finite, bounded output and some output for an impulse and a noise
// burst within one second.
static void checkAllPrograms() {
    int ran = 0;
    for (int k = 0; k < 3; k++) {
        const midiverb::RomInfo& r = midiverb::rom(k);
        for (int p = r.firstProgram; p <= r.lastProgram; p++) {
            midiverb::Emulator e;
            e.selectRom(k);
            e.selectProgram(p);
            e.dryWetMix = 1;
            uint32_t s = 7;
            double peak = 0, energy = 0;
            bool finite = true;
            for (int i = 0; i < 48000; i++) {  // 1 s: the longest echoes are about 460 ms
                double l, rr, ol, orr;
                input(i, s, l, rr);
                e.process(l, rr, ol, orr);
                finite &= std::isfinite(ol) && std::isfinite(orr);
                peak = std::max(peak, std::max(std::fabs(ol), std::fabs(orr)));
                energy += ol * ol + orr * orr;
            }
            CHECK(finite && peak < 2.0, "%s %d: finite %d peak %g", r.name, p, finite, peak);
            CHECK(energy > 0, "%s %d: no output at all", r.name, p);
            ran++;
        }
    }
    std::printf("%d programs run\n", ran);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
        std::printf("# this port's output for the cases of test/midiverb_ref.js\n");
        for (const Case& c : cases()) {
            Result r = run(c);
            std::printf("case %s\n", c.name);
            for (const auto& l : r.lines) std::printf("%s\n", l.c_str());
            std::printf("hash %016llx\n", (unsigned long long)r.hash);
        }
        return 0;
    }
    checkTables();
    checkAllPrograms();

    // reverb tails decay with no feedback, and the cases run clean
    for (const Case& c : cases()) {
        Result r = run(c);
        CHECK(r.finite && r.peak < 2.0, "%s: finite %d peak %g", c.name, r.finite, r.peak);
        if (c.fb == 0 && c.changes.empty() && std::strcmp(c.name, "mfx_p58_thick") != 0)
            CHECK(r.peakTail < 0.05, "%s: tail at the end %g", c.name, r.peakTail);
    }

    const char* path = argc > 1 ? argv[1] : "expected/midiverb.txt";
    std::ifstream in(path);
    if (!in) {
        std::printf("no %s: bit-exact comparison against the JavaScript SKIPPED (see test/midiverb_ref.js)\n", path);
    } else {
        std::vector<Case> cs = cases();
        size_t ci = 0;
        std::string line;
        Result cur;
        size_t li = 0;
        int compared = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("case ", 0) == 0) {
                std::string name = line.substr(5);
                CHECK(ci < cs.size() && name == cs[ci].name, "case order: %s", name.c_str());
                if (ci >= cs.size()) break;
                cur = run(cs[ci++]);
                li = 0;
            } else if (line.rfind("hash ", 0) == 0) {
                unsigned long long want = std::strtoull(line.c_str() + 5, nullptr, 16);
                CHECK(cur.hash == want, "%s: hash %016llx, JavaScript %016llx", cs[ci - 1].name, (unsigned long long)cur.hash, want);
                compared++;
            } else {
                int i;
                double l, r;
                std::istringstream ss(line);
                ss >> i >> l >> r;
                if (li < cur.l.size())
                    CHECK(cur.l[li] == l && cur.r[li] == r, "%s sample %d: %.17g %.17g, JavaScript %.17g %.17g", cs[ci - 1].name, i, cur.l[li], cur.r[li], l, r);
                li++;
            }
        }
        std::printf("%d cases compared bit for bit against the JavaScript\n", compared);
    }
    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::printf("midiverb test passed\n");
    return 0;
}
