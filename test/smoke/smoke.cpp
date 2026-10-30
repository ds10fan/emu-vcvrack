// Headless smoke test of the Rack module: constructs HChipFilter, drives process() with a sine
// and checks that the stock lowpass passes a low tone and stops a high one, that polyphony and
// the type switch work, and reports the cost per sample. Links against the SDK's libRack.so.
#include <chrono>
#include <cstring>
#include <fstream>
#include <cmath>
#include <cstdio>

#include "../../src/GChip.cpp"
#include "../../src/HChipFilter.cpp"

Plugin* pluginInstance = nullptr;

static double gainDb(HChipFilter& m, float freq, int channels, float amp = 4.f) {
    const float fs = 48000.f;
    Module::ProcessArgs args;
    args.sampleRate = fs;
    args.sampleTime = 1.f / fs;
    // Port::setChannels is a no-op while disconnected (the engine sets it from the cable).
    m.inputs[HChipFilter::IN_INPUT].channels = channels;
    m.outputs[HChipFilter::OUT_OUTPUT].channels = 1;  // "connected", so the module's setChannels takes
    const int settle = 4096, n = 8192;
    double re = 0, im = 0;
    for (int i = 0; i < settle + n; i++) {
        args.frame = i;
        float x = amp * std::sin(2 * M_PI * freq * i / fs);
        for (int c = 0; c < channels; c++) m.inputs[HChipFilter::IN_INPUT].setVoltage(x, c);
        m.process(args);
        float y = m.outputs[HChipFilter::OUT_OUTPUT].getVoltage(channels - 1);
        if (i >= settle) {
            re += y * std::cos(-2 * M_PI * freq * i / fs);
            im += y * std::sin(-2 * M_PI * freq * i / fs);
        }
    }
    return 20 * std::log10(std::hypot(re, im) / (amp * n / 2.0));
}

// A 16-bit mono WAV of a sine with an smpl loop, for the sampler test.
static std::string writeTestWav() {
    std::vector<unsigned char> b;
    auto put16 = [&](unsigned v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); };
    auto put32 = [&](unsigned v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 255); };
    auto tag = [&](const char* t) { b.insert(b.end(), t, t + 4); };
    const int n = 4800;
    tag("RIFF"); put32(0); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(1); put32(48000); put32(96000); put16(2); put16(16);
    tag("smpl"); put32(60); for (int i = 0; i < 7; i++) put32(0); put32(1); put32(0);
    put32(0); put32(0); put32(1000); put32(1999); put32(0); put32(0);
    tag("data"); put32(n * 2);
    for (int i = 0; i < n; i++) put16((unsigned)(uint16_t)(int16_t)std::lround(20000 * std::sin(2 * M_PI * 100.0 * i / 48000.0)));
    unsigned riff = b.size() - 8;
    for (int i = 0; i < 4; i++) b[4 + i] = (riff >> (8 * i)) & 255;
    std::string path = "build/smoke_test.wav";
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
    return path;
}

static int smokeGChip() {
    int fails = 0;
    std::string path = writeTestWav();
    GChip g;
    if (!g.loadSample(path, false)) {
        std::printf("gchip: load failed: %s\n", g.loadError.c_str());
        return 1;
    }
    Module::ProcessArgs args;
    args.sampleRate = 48000.f;
    args.sampleTime = 1.f / 48000.f;
    g.inputs[GChip::GATE_INPUT].channels = 4;
    g.outputs[GChip::OUT_OUTPUT].channels = 1;
    g.outputs[GChip::EOC_OUTPUT].channels = 1;
    // One-shot at the original pitch: 4800 samples of a 100 Hz sine, then silence and an EOC pulse.
    // (Rack's SchmittTrigger starts high, so the gate must be seen low once before it can fire.)
    g.params[GChip::LOOP_MODE_PARAM].setValue(0);
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(0.f, c);
    g.process(args);
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(10.f, c);
    double peak = 0, re = 0, im = 0;
    int eocs = 0;
    bool eocHigh = false;
    for (int i = 0; i < 6000; i++) {
        g.process(args);
        float y = g.outputs[GChip::OUT_OUTPUT].getVoltage(3);
        if (i >= 100 && i < 4420) {  // 4320 samples: exactly 9 periods of 100 Hz at 48 kHz
            peak = std::max(peak, (double)std::fabs(y));
            re += y * std::cos(2 * M_PI * 100.0 * (i + 0.5) / 48000.0);
            im += y * std::sin(2 * M_PI * 100.0 * (i + 0.5) / 48000.0);
        }
        if (i >= 4900 && y != 0.f) fails++;
        bool hi = g.outputs[GChip::EOC_OUTPUT].getVoltage(3) > 5.f;
        if (hi && !eocHigh) eocs++;
        eocHigh = hi;
    }
    double amp = std::hypot(re, im) / 2160.0;  // (4320 samples) / 2
    std::printf("gchip one-shot: peak %.3f V, 100 Hz amplitude %.3f V (want 3.052), EOC pulses %d, channels %d\n", peak, amp,
                eocs, g.outputs[GChip::OUT_OUTPUT].getChannels());
    if (std::fabs(amp - 20000.0 / 32768.0 * 5.0) > 0.02) fails++;
    if (eocs != 1) fails++;
    if (g.outputs[GChip::OUT_OUTPUT].getChannels() != 4) fails++;
    // Loop On, an octave down: still sounding long after the sample length.
    g.params[GChip::LOOP_MODE_PARAM].setValue(2);
    g.params[GChip::LOOP_START_PARAM].setValue(1000.f / 4800.f);
    g.params[GChip::LOOP_END_PARAM].setValue(2000.f / 4800.f);
    g.inputs[GChip::VOCT_INPUT].channels = 1;
    g.inputs[GChip::VOCT_INPUT].setVoltage(-1.f, 0);
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(0.f, c);
    g.process(args);
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(10.f, c);
    peak = 0;
    for (int i = 0; i < 48000; i++) {
        g.process(args);
        if (i > 40000) peak = std::max(peak, (double)std::fabs(g.outputs[GChip::OUT_OUTPUT].getVoltage(0)));
    }
    std::printf("gchip loop on, -1 V: peak after 40000 samples %.3f V\n", peak);
    if (peak < 2.5) fails++;
    // Gate mode: release, then it must end.
    g.params[GChip::LOOP_MODE_PARAM].setValue(1);
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(0.f, c);
    g.process(args);
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(10.f, c);
    for (int i = 0; i < 20000; i++) g.process(args);
    bool stillOn = g.voices[0].active();
    for (int c = 0; c < 4; c++) g.inputs[GChip::GATE_INPUT].setVoltage(0.f, c);
    int n = 0;
    while (g.voices[0].active() && n < 20000) {
        g.process(args);
        n++;
    }
    std::printf("gchip gate mode: looping while held %d, ended %d samples after release\n", stillOn, n);
    if (!stillOn || n >= 20000) fails++;
    return fails;
}

int main() {
    int fails = smokeGChip();
    HChipFilter m;
    // 2 Pole Low-pass, Q 0, fully open: flat (the gain word 0xFE, -0.14 dB)
    m.params[HChipFilter::TYPE_PARAM].setValue(0);
    m.params[HChipFilter::Q_PARAM].setValue(0);
    m.params[HChipFilter::CUTOFF_PARAM].setValue(255);
    double g = gainDb(m, 1000, 1);
    std::printf("lp2 q0 open, 1 kHz: %.2f dB\n", g);
    if (std::fabs(g + 0.14) > 0.1) fails++;
    if (m.outputs[HChipFilter::OUT_OUTPUT].getChannels() != 1) fails++;
    // closed: 1 kHz well down
    m.params[HChipFilter::CUTOFF_PARAM].setValue(0);
    g = gainDb(m, 1000, 1);
    std::printf("lp2 q0 closed, 1 kHz: %.2f dB\n", g);
    if (g > -30) fails++;
    // Q 64 at cutoff 128: +7.7 dB at 1 kHz (docs/VOICE.md), with 16-bit LATCH 18 and a small input
    m.params[HChipFilter::Q_PARAM].setValue(64);
    m.params[HChipFilter::CUTOFF_PARAM].setValue(128);
    g = gainDb(m, 1000, 1, 1.5f);
    std::printf("lp2 q64 c128, 1 kHz: %.2f dB\n", g);
    if (std::fabs(g - 7.75) > 0.2) fails++;
    // 4th Order High-pass, Q 64, cutoff 128, 16 channels: 100 Hz down, 5 kHz near 0 dB
    m.params[HChipFilter::TYPE_PARAM].setValue(4);
    double lo = gainDb(m, 100, 16, 1.5f), hi = gainDb(m, 5000, 16, 1.5f);
    std::printf("hp4 q64 c128 (16 ch), 100 Hz: %.2f dB, 5 kHz: %.2f dB\n", lo, hi);
    if (!(lo < -30 && std::fabs(hi - 0.86) < 0.3)) fails++;
    if (m.outputs[HChipFilter::OUT_OUTPUT].getChannels() != 16) fails++;
    // every type runs
    for (int t = 0; t < kNumTypes; t++) {
        m.params[HChipFilter::TYPE_PARAM].setValue(t);
        double g2 = gainDb(m, 1000, 1, 1.f);
        if (!std::isfinite(g2)) {
            std::printf("type %d: non-finite output\n", t);
            fails++;
        }
    }
    // cost: 16 channels of the 6-pole lowpass with a moving cutoff
    m.params[HChipFilter::TYPE_PARAM].setValue(2);
    Module::ProcessArgs args;
    args.sampleRate = 48000.f;
    args.sampleTime = 1.f / 48000.f;
    m.inputs[HChipFilter::IN_INPUT].channels = 16;
    m.inputs[HChipFilter::CUTOFF_INPUT].channels = 1;
    const int N = 48000;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) {
        float x = 3.f * std::sin(2 * M_PI * 220.0 * i / 48000.0);
        for (int c = 0; c < 16; c++) m.inputs[HChipFilter::IN_INPUT].setVoltage(x, c);
        m.inputs[HChipFilter::CUTOFF_INPUT].setVoltage(5.f * std::sin(2 * M_PI * 0.5 * i / 48000.0), 0);
        m.process(args);
    }
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    std::printf("16 channels, 1 s of audio: %.0f ms (%.1f%% of real time at 48 kHz)\n", us / 1000, us / 1e4);
    std::printf(fails ? "%d FAILURE(S)\n" : "smoke test passed\n", fails);
    return fails ? 1 : 0;
}
