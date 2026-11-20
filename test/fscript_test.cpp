// Tests of the fscript language and the Filter Lab factory models.
#include <cmath>
#include <cstdio>
#include <string>

#include "../dsp/filterlab_presets.hpp"
#include "../dsp/fscript.hpp"
#include "../dsp/oversampler.hpp"
#include "../dsp/response.hpp"
#include <chrono>

using namespace fscript;

static int failures = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            failures++;                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);         \
            std::printf("\n");                \
        }                                     \
    } while (0)

static bool build(const std::string& src, Program& p, Voice& v, double sr = 48000) {
    Error e;
    if (!compile(src, p, e)) {
        std::printf("  compile error %d:%d %s\n", e.line, e.col, e.msg.c_str());
        return false;
    }
    prepare(p, v);
    reset(p, v, sr, 0);
    return true;
}

static double eval1(const std::string& src, double in = 0) {
    Program p;
    Voice v;
    if (!build(src, p, v)) return NAN;
    v.mem[B_IN] = in;
    run(p, v);
    return v.mem[B_OUT];
}

static bool compileFails(const std::string& src, int line) {
    Program p;
    Error e;
    bool ok = compile(src, p, e);
    if (!ok && line && e.line != line) std::printf("  error on line %d, expected %d: %s\n", e.line, line, e.msg.c_str());
    return !ok && (line == 0 || e.line == line);
}

static void language() {
    CHECK(eval1("out = 1 + 2 * 3") == 7, "precedence");
    CHECK(eval1("out = -2 ^ 2") == -4, "unary minus and power");
    CHECK(eval1("out = 2 ^ 3 ^ 2") == 512, "power right assoc");
    CHECK(eval1("out = 7 % 3") == 1 && eval1("out = -1 % 3") == 2, "floored modulo");
    CHECK(eval1("out = 1 < 2 && 3 > 2 ? 5 : 6") == 5, "ternary, logic");
    CHECK(eval1("out = in * 2", 0.25) == 0.5, "input");
    CHECK(eval1("x = 3; x += 2; x *= 2; out = x") == 10, "compound assign");
    CHECK(eval1("const n = 4 * 2\nout = n") == 8, "const");
    CHECK(eval1("fn sq(x) = x * x\nfn f(a, b) = sq(a) + b\nout = f(3, 1)") == 10, "functions");
    CHECK(eval1("s = 0\nfor i in 0..5 { s += i }\nout = s") == 10, "for");
    CHECK(eval1("x = 2\nif x > 1 { out = 1 } else if x > 0 { out = 2 } else { out = 3 }") == 1, "if");
    CHECK(eval1("x = 0.5\nif x > 1 { out = 1 } else if x > 0 { out = 2 } else { out = 3 }") == 2, "else if");
    CHECK(eval1("if 0 out = 1 else out = 4") == 4, "if without braces");
    CHECK(eval1("buf t = [1, -2, 3]\nout = t[1] + t[4] + len(t)") == -1, "table, wrap, len");
    CHECK(eval1("buf d[4]\nd[1] = 2; d[2] = 4\nout = tap(d, 1.25)") == 2.5, "tap");
    CHECK(eval1("init { k = 3 }\nout = k + sr / 48000") == 4, "init, sr");
    CHECK(std::fabs(eval1("out = warp(12000)") - 1.0) < 1e-12, "warp");
    CHECK(std::fabs(eval1("out = hz(0.5)") - 20 * std::sqrt(1000.0)) < 1e-9, "hz");
    CHECK(eval1("out = 1.5e1 + .5") == 15.5, "numbers");
    CHECK(eval1("/* c */ out = 1 // c") == 1, "comments");

    // four inputs and outputs, in1/out1 aliases
    {
        Program p;
        Voice v;
        build("out = in1 + 1\nout2 = in2 * 2\nout3 = in3 - in4\nout4 = in4\nout1 += 10", p, v);
        v.mem[B_IN] = 1;
        v.mem[B_IN2] = 2;
        v.mem[B_IN3] = 3;
        v.mem[B_IN4] = 4;
        run(p, v);
        CHECK(v.mem[B_OUT] == 12 && v.mem[B_OUT2] == 4 && v.mem[B_OUT3] == -1 && v.mem[B_OUT4] == 4, "4 in / 4 out");
        CHECK(compileFails("in3 = 1", 1), "assign in3");
        CHECK(compileFails("#out5 x\nout = 0", 1), "no out5");
    }
    // state persists
    {
        Program p;
        Voice v;
        build("n += 1\nout = n", p, v);
        for (int i = 0; i < 5; i++) run(p, v);
        CHECK(v.mem[B_OUT] == 5, "state persists");
    }
    // loop budget
    {
        Program p;
        Voice v;
        build("for i in 0..1e9 { x = i }\nout = 1", p, v);
        CHECK(!run(p, v), "loop budget stops a runaway loop");
    }
    // live edit keeps state, except init-assigned and table buffers
    {
        Program a, b;
        Voice va, vb;
        build("init { g = 1 }\nbuf t = [1, 2]\nn += 1\nout = n", a, va);
        for (int i = 0; i < 3; i++) run(a, va);
        build("init { g = 2 }\nbuf t = [5, 6]\nn += 10\nout = n + g + t[0]", b, vb);
        transfer(a, va, b, vb);
        run(b, vb);
        CHECK(vb.mem[B_OUT] == 13 + 2 + 5, "transfer: %g", vb.mem[B_OUT]);
    }
    // header
    {
        Program p;
        Error e;
        compile("#name Foo bar\n#p2  Cut off \n#out2 BP\n#in Main\n#in4 Side\n#out1 LP\n#out4 Notch\nout = 0", p, e);
        CHECK(p.name == "Foo bar" && p.paramLabel[1] == "Cut off" && p.outLabel[1] == "BP", "directives");
        CHECK(p.inLabel[0] == "Main" && p.inLabel[3] == "Side" && p.inLabel[1].empty(), "input labels");
        CHECK(p.outLabel[0] == "LP" && p.outLabel[3] == "Notch" && p.outLabel[2].empty(), "output labels");
    }
    // errors carry the right line
    CHECK(compileFails("out = 1\nout = cutof", 2), "unknown variable");
    CHECK(compileFails("out = 1\n\nin = 2", 3), "assign input");
    CHECK(compileFails("out = (1", 1), "missing paren");
    CHECK(compileFails("out = foo(1)", 1), "unknown function");
    CHECK(compileFails("out = sin(1, 2)", 1), "arity");
    CHECK(compileFails("x = 1\nif x { buf d[3] }", 2), "nested buf");
    CHECK(compileFails("buf d[0]", 1), "buffer size");
    CHECK(compileFails("#foo\nout = 1", 1), "directive");
    CHECK(compileFails("fn f(x) = f(x)\nout = f(1)", 0), "recursion");
    CHECK(compileFails("out = 1 $", 1), "bad character");
    CHECK(compileFails("const k = in", 1), "const of input");
    CHECK(compileFails("x = 1\nsin(x)", 2), "call as statement");
    CHECK(compileFails("for i in 0..3 { x = i", 1), "missing brace");
}

// RMS gain of a sine through the model at frequency f.
static double gain(const Program& p, double f, double p1, double p2, double p3, double p4, double sr = 48000,
                   bool second = false) {
    Voice v;
    prepare(p, v);
    v.mem[B_P1] = p1;
    v.mem[B_P2] = p2;
    v.mem[B_P3] = p3;
    v.mem[B_P4] = p4;
    reset(p, v, sr, 0);
    int n = (int)sr / 2;
    double si = 0, so = 0;
    for (int i = 0; i < n; i++) {
        double x = 0.1 * std::sin(2 * M_PI * f * i / sr);
        v.mem[B_IN] = x;
        v.mem[B_P1] = p1;
        v.mem[B_P2] = p2;
        v.mem[B_P3] = p3;
        v.mem[B_P4] = p4;
        run(p, v);
        if (i >= n / 2) {
            double y = v.mem[second ? B_OUT2 : B_OUT];
            si += x * x;
            so += y * y;
        }
    }
    return std::sqrt(so / si);
}

static double dB(double g) { return 20 * std::log10(g + 1e-30); }

static void presets() {
    for (int i = 0; i < filterlab::kPresetCount; i++) {
        const filterlab::Preset& pr = filterlab::kPresets[i];
        Program p;
        Error e;
        bool ok = compile(pr.code, p, e);
        CHECK(ok, "preset %s: %d:%d %s", pr.name, e.line, e.col, e.msg.c_str());
        if (!ok) continue;
        CHECK(!p.name.empty(), "preset %s has #name", pr.name);

        // Stress: noise and loud square through every corner of the knobs, at 48 and 192 kHz.
        double srs[] = {48000, 192000};
        for (double sr : srs) {
            for (int corner = 0; corner < 16; corner++) {
                Voice v;
                prepare(p, v);
                double k[4];
                for (int j = 0; j < 4; j++) k[j] = (corner >> j) & 1 ? 1.0 : 0.0;
                for (int j = 0; j < 4; j++) v.mem[B_P1 + j] = k[j];
                reset(p, v, sr, 0);
                uint32_t r = 1;
                double peak = 0;
                bool finite = true, budget = true;
                for (int n = 0; n < 20000; n++) {
                    r = r * 1664525u + 1013904223u;
                    double x = n < 10000 ? ((r >> 8) / 8388608.0 - 1.0) * 2 : ((n / 50) & 1 ? 2.0 : -2.0);
                    v.mem[B_IN] = x;
                    v.mem[B_IN2] = x;
                    v.mem[B_IN3] = x;
                    v.mem[B_IN4] = -x;
                    for (int j = 0; j < 4; j++) v.mem[B_P1 + j] = k[j];
                    budget &= run(p, v);
                    for (int j = B_OUT; j <= B_OUT4; j++) {
                        double o = v.mem[j];
                        if (!std::isfinite(o)) finite = false;
                        peak = std::fmax(peak, std::fabs(o));
                    }
                }
                CHECK(finite && budget, "preset %s sr %g corner %d: not finite", pr.name, sr, corner);
                CHECK(peak < 50, "preset %s sr %g corner %d: peak %g", pr.name, sr, corner, peak);
            }
        }
    }
}

static const Program* find(const char* name, Program& p) {
    for (int i = 0; i < filterlab::kPresetCount; i++) {
        if (std::string(filterlab::kPresets[i].name) == name) {
            Error e;
            compile(filterlab::kPresets[i].code, p, e);
            return &p;
        }
    }
    std::printf("no preset %s\n", name);
    failures++;
    return nullptr;
}

static void responses() {
    Program p;
    // cutoff 0.5 = hz(0.5) = 632 Hz
    if (find("One-Pole", p)) {
        CHECK(std::fabs(dB(gain(p, 632.5, 0.5, 0, 0, 0)) + 3.01) < 0.2, "one-pole -3 dB at cutoff: %g",
              dB(gain(p, 632.5, 0.5, 0, 0, 0)));
        CHECK(std::fabs(dB(gain(p, 6325, 0.5, 0, 0, 0)) + 20) < 1, "one-pole -20 dB a decade up");
    }
    if (find("TPT SVF", p)) {
        double pass = dB(gain(p, 100, 0.5, 0.3, 0, 0)), stop = dB(gain(p, 6325, 0.5, 0.3, 0, 0));
        CHECK(std::fabs(pass) < 0.5 && stop < -35, "svf LP pass %g stop %g", pass, stop);
        double hpp = dB(gain(p, 6325, 0.5, 0.3, 1, 0)), hps = dB(gain(p, 63, 0.5, 0.3, 1, 0));
        CHECK(std::fabs(hpp) < 0.5 && hps < -35, "svf HP pass %g stop %g", hpp, hps);
        double bp = dB(gain(p, 632.5, 0.5, 0.3, 0.5, 0));
        CHECK(std::fabs(bp) < 0.3, "svf BP unity at centre %g", bp);
        double res = dB(gain(p, 632.5, 0.5, 0.9, 0, 0));
        CHECK(res > 12, "svf resonance peak %g", res);
    }
    if (find("RBJ Biquad", p)) {
        CHECK(std::fabs(dB(gain(p, 632.5, 0.5, 0.0, 0.0, 0)) + 6.02) < 0.3, "rbj LP Q 0.5 at cutoff: %g",
              dB(gain(p, 632.5, 0.5, 0.0, 0.0, 0)));
        CHECK(dB(gain(p, 6325, 0.5, 0.1, 0.0, 0)) < -35, "rbj LP stop");
        CHECK(dB(gain(p, 63, 0.5, 0.1, 0.3, 0)) < -35, "rbj HP stop");
        CHECK(std::fabs(dB(gain(p, 632.5, 0.5, 0.3, 0.5, 0))) < 0.2, "rbj BP centre");
        CHECK(dB(gain(p, 632.5, 0.5, 0.3, 0.7, 0)) < -40, "rbj notch");
        CHECK(std::fabs(dB(gain(p, 632.5, 0.5, 0.3, 0.9, 1.0)) - 18) < 0.3, "rbj peak +18: %g",
              dB(gain(p, 632.5, 0.5, 0.3, 0.9, 1.0)));
    }
    if (find("Butterworth 24", p)) {
        double c = dB(gain(p, 632.5, 0.5, 0, 0, 0)), o = dB(gain(p, 1265, 0.5, 0, 0, 0));
        CHECK(std::fabs(c + 3) < 0.3 && std::fabs(o + 24.1) < 1, "butterworth LP %g %g", c, o);
        double h = dB(gain(p, 632.5, 0.5, 1, 0, 0)), ho = dB(gain(p, 316, 0.5, 1, 0, 0));
        CHECK(std::fabs(h + 3) < 0.3 && std::fabs(ho + 24.1) < 1, "butterworth HP %g %g", h, ho);
    }
    if (find("ZDF Ladder", p)) {
        double pass = dB(gain(p, 50, 0.5, 0, 0, 0)), stop = dB(gain(p, 6325, 0.5, 0, 0, 0));
        CHECK(std::fabs(pass) < 0.5 && stop < -70, "zdf ladder pass %g stop %g", pass, stop);
    }
    if (find("Moog Ladder", p)) {
        double pass = dB(gain(p, 50, 0.5, 0, 0, 0)), stop = dB(gain(p, 6325, 0.5, 0, 0, 0));
        CHECK(std::fabs(pass) < 1 && stop < -60, "moog ladder pass %g stop %g", pass, stop);
        // tuning: response near cutoff around -12 dB for a 4-pole w/o resonance (approx)
        double c = dB(gain(p, 632.5, 0.5, 0, 0, 0));
        CHECK(c < -6 && c > -18, "moog ladder at cutoff %g", c);
    }
    if (find("Korg35", p)) {
        double pass = dB(gain(p, 50, 0.5, 0.2, 0, 0)), stop = dB(gain(p, 6325, 0.5, 0.2, 0, 0));
        CHECK(pass > -3 && stop < -30, "korg35 pass %g stop %g", pass, stop);
    }
    if (find("Chamberlin SVF", p)) {
        double pass = dB(gain(p, 50, 0.5, 0.3, 0, 0)), stop = dB(gain(p, 6325, 0.5, 0.3, 0, 0));
        CHECK(std::fabs(pass) < 0.5 && stop < -35, "chamberlin pass %g stop %g", pass, stop);
    }
    if (find("Formant", p)) {
        double f1 = dB(gain(p, 800, 0.0, 0.3, 0.5, 0)), off = dB(gain(p, 5000, 0.0, 0.3, 0.5, 0));
        CHECK(f1 > off + 15, "formant peak %g vs %g", f1, off);
    }
}

static void oversampler() {
    // a 1 kHz sine through up and down at each factor comes back with unity gain
    int factors[] = {1, 2, 4, 8};
    for (int f : factors) {
        filterlab::Oversampler os;
        os.setFactor(f);
        double sum = 0, sumIn = 0;
        float up[8];
        for (int n = 0; n < 48000; n++) {
            float x = std::sin(2 * M_PI * 1000 * n / 48000.0);
            os.up(x, up);
            float y = os.down(up);
            if (n > 1000) {
                sum += y * y;
                sumIn += x * x;
            }
        }
        double g = std::sqrt(sum / sumIn);
        CHECK(std::fabs(g - 1) < 0.01, "oversampler x%d gain %g", f, g);
    }
}

// The impulse response analysis agrees with the sine measurements above.
static void analysis() {
    Program p;
    const double cv[4] = {0, 0, 0, 0};
    struct Case {
        const char* name;
        double k[4];
    } cases[] = {{"One-Pole", {0.5, 0, 0, 0}}, {"RBJ Biquad", {0.5, 0.3, 0.9, 1.0}}, {"Butterworth 24", {0.5, 0, 0, 0}}};
    const double freqs[] = {63, 316, 632.5, 1265, 6325};
    for (const Case& c : cases) {
        if (!find(c.name, p)) continue;
        int factors[] = {1, 4};
        for (int os : factors) {
            filterlab::Response r;
            CHECK(filterlab::analyse(p, c.k, cv, 48000, os, 1.0, 8192, r), "analyse %s x%d", c.name, os);
            for (double f : freqs) {
                double want = dB(gain(p, f, c.k[0], c.k[1], c.k[2], c.k[3])), got = dB(r.at(0, f));
                if (want < -70) continue;
                CHECK(std::fabs(want - got) < 0.5, "analyse %s x%d at %g Hz: %g, sine %g", c.name, os, f, got, want);
            }
            CHECK(r.active[0] && !r.active[3], "analyse %s: active outputs", c.name);
        }
    }
    // a program that blows up is reported, not plotted
    Program bad;
    Voice v;
    if (build("y = y * 4 + in\nout = 1 / (y - y)", bad, v)) {
        filterlab::Response r;
        const double k[4] = {0, 0, 0, 0};
        CHECK(!filterlab::analyse(bad, k, cv, 48000, 1, 1.0, 8192, r) && !r.ok, "analyse: blow-up not reported");
    }
    // cost of one analysis of the heaviest factory model, for the record
    double worst = 0;
    const char* worstName = "";
    for (int i = 0; i < filterlab::kPresetCount; i++) {
        Program q;
        Error e;
        if (!compile(filterlab::kPresets[i].code, q, e)) continue;
        const double k[4] = {0.5, 0.5, 0.5, 0.5};
        filterlab::Response r;
        auto t0 = std::chrono::steady_clock::now();
        filterlab::analyse(q, k, cv, 48000, 1, 1.0, 8192, r);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > worst) {
            worst = ms;
            worstName = filterlab::kPresets[i].name;
        }
    }
    std::printf("  analyse 8192 samples x1: worst %.2f ms (%s)\n", worst, worstName);
}

int main() {
    language();
    presets();
    responses();
    oversampler();
    analysis();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("fscript: all tests passed (%d models)\n", filterlab::kPresetCount);
    return 0;
}
