// Headless smoke test of the Rack module: constructs HChipFilter, drives process() with a sine
// and checks that the stock lowpass passes a low tone and stops a high one, that polyphony and
// the type switch work, and reports the cost per sample. Links against the SDK's libRack.so.
#include <chrono>
#include <cmath>
#include <cstdio>

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

int main() {
    int fails = 0;
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
