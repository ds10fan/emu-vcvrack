// Headless smoke test of the Rack module: constructs HChipFilter, drives process() with a sine
// and checks that the stock lowpass passes a low tone and stops a high one, that polyphony and
// the type switch work, and reports the cost per sample. Links against the SDK's libRack.so.
#include <chrono>
#include <cstring>
#include <fstream>
#include <cmath>
#include <cstdio>

#include "../../src/GChip.cpp"
#include "../../src/MintySeq.cpp"
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

static int smokeMintySeq() {
    int fails = 0;
    MintySeq m;
    Module::ProcessArgs args;
    args.sampleRate = 48000.f;
    args.sampleTime = 1.f / 48000.f;
    auto press = [&](int param) {
        m.params[param].setValue(1.f);
        m.process(args);
        m.params[param].setValue(0.f);
        m.process(args);
    };
    // Run until the play head arrives at `step` afresh (a pattern or set change applies at the
    // next step boundary, so being there already does not count).
    auto advanceTo = [&](int step) {
        for (int i = 0; i < 70 * 5670 && m.seq.pos() == step; i++) m.process(args);
        for (int i = 0; i < 70 * 5670 && m.seq.pos() != step; i++) m.process(args);
    };
    // Rack's BooleanTrigger starts high: one process() with the buttons up before the first press.
    m.process(args);
    // REC arms and starts
    press(MintySeq::REC_PARAM);
    if (!(m.seq.running && m.seq.recording)) fails++;
    // a 2 V note from step 4 held 1.6 steps (nearest: two steps); while held the selected track monitors it
    advanceTo(4);
    m.inputs[MintySeq::GATE_INPUT].setVoltage(10.f);
    m.inputs[MintySeq::CV_INPUT].setVoltage(2.f);
    m.process(args);
    bool monitor = m.outputs[MintySeq::CV_OUTPUT].getVoltage() == 2.f && m.outputs[MintySeq::GATE_OUTPUT].getVoltage() == 10.f;
    for (int i = 0; i < int(1.6 * 5669); i++) m.process(args);
    m.inputs[MintySeq::GATE_INPUT].setVoltage(0.f);
    m.process(args);
    const minty::Pattern& p = m.seq.currentPattern();
    bool recorded = p.steps[4].gate && p.steps[4].cv == 2.f && p.steps[5].tie && !p.steps[6].tie && !p.steps[6].gate;
    std::printf("minty: monitor %d, recorded note at step 4 len 2: %d\n", monitor, recorded);
    if (!monitor || !recorded) fails++;
    // stop, start, play back: step 4 sounds 2 V, step 6 is silent, track 2 silent
    press(MintySeq::PLAY_PARAM);
    if (m.seq.running || m.outputs[MintySeq::GATE_OUTPUT].getVoltage() != 0.f) fails++;
    press(MintySeq::PLAY_PARAM);
    advanceTo(4);
    for (int i = 0; i < 100; i++) m.process(args);  // past the retrigger gap
    bool play4 = m.outputs[MintySeq::CV_OUTPUT].getVoltage() == 2.f && m.outputs[MintySeq::GATE_OUTPUT].getVoltage() == 10.f &&
                 m.outputs[MintySeq::GATE_OUTPUT + 1].getVoltage() == 0.f;
    advanceTo(6);
    bool play6 = m.outputs[MintySeq::GATE_OUTPUT].getVoltage() == 0.f;
    std::printf("minty: playback step 4 %d, step 6 silent %d\n", play4, play6);
    if (!play4 || !play6) fails++;
    // click: a burst right after a beat (step 8), silence just before the next beat (step 12),
    // the bar click (step 0) louder than a beat click
    auto clickPeak = [&](int samples) {
        float pk = 0.f;
        for (int i = 0; i < samples; i++) {
            m.process(args);
            pk = std::max(pk, std::fabs(m.outputs[MintySeq::CLICK_OUTPUT].getVoltage()));
        }
        return pk;
    };
    advanceTo(8);
    float beatPeak = clickPeak(200);
    advanceTo(11);
    for (int i = 0; i < 5000; i++) m.process(args);
    float quietPeak = clickPeak(100);
    advanceTo(0);
    float barPeak = clickPeak(200);
    advanceTo(1);
    float offBeat = clickPeak(200);
    std::printf("minty: click beat %.2f V, before next beat %.3f V, bar %.2f V, off-beat step %.3f V\n", beatPeak, quietPeak, barPeak, offBeat);
    if (!(beatPeak > 0.5f && quietPeak < 0.05f && barPeak > beatPeak && offBeat < 0.05f)) fails++;
    // A/B: B is empty
    press(MintySeq::AB_PARAM);
    advanceTo(4);
    for (int i = 0; i < 100; i++) m.process(args);
    bool bSilent = m.seq.ab(0) == 1 && m.outputs[MintySeq::GATE_OUTPUT].getVoltage() == 0.f;
    press(MintySeq::AB_PARAM);
    // sets: set 2 is empty, back to set 1 plays
    press(MintySeq::SET_NEXT_PARAM);
    advanceTo(4);
    for (int i = 0; i < 100; i++) m.process(args);
    bool set2Silent = m.seq.currentSet == 1 && m.outputs[MintySeq::GATE_OUTPUT].getVoltage() == 0.f;
    press(MintySeq::SET_PREV_PARAM);
    advanceTo(4);
    for (int i = 0; i < 100; i++) m.process(args);
    bool set1Plays = m.seq.currentSet == 0 && m.outputs[MintySeq::GATE_OUTPUT].getVoltage() == 10.f;
    std::printf("minty: B silent %d, set 2 silent %d, set 1 plays %d\n", bSilent, set2Silent, set1Plays);
    if (!bSilent || !set2Silent || !set1Plays) fails++;
    // keyboard requests and clear
    m.request(MintySeq::REQ_TRACK_DOWN);
    m.process(args);
    bool track2 = m.seq.currentTrack == 1;
    m.request(MintySeq::REQ_TRACK_UP);
    m.process(args);
    press(MintySeq::CLEAR_PARAM);
    bool cleared = m.seq.currentTrack == 0 && p.empty();
    std::printf("minty: key track down %d, clear %d\n", track2, cleared);
    if (!track2 || !cleared) fails++;
    // external clock: steps advance only on pulses
    m.inputs[MintySeq::CLOCK_INPUT].channels = 1;
    m.request(MintySeq::REQ_RUN);
    m.process(args);  // stop
    m.request(MintySeq::REQ_RUN);
    m.process(args);  // start: waits for a pulse
    for (int i = 0; i < 20000; i++) m.process(args);
    int before = m.seq.pos();
    m.inputs[MintySeq::CLOCK_INPUT].setVoltage(10.f);
    m.process(args);
    m.inputs[MintySeq::CLOCK_INPUT].setVoltage(0.f);
    m.process(args);
    int afterFirst = m.seq.pos();
    m.inputs[MintySeq::CLOCK_INPUT].setVoltage(10.f);
    m.process(args);
    m.inputs[MintySeq::CLOCK_INPUT].setVoltage(0.f);
    m.process(args);
    int afterSecond = m.seq.pos();
    std::printf("minty: external clock positions %d -> %d -> %d\n", before, afterFirst, afterSecond);
    if (!(before == 0 && afterFirst == 0 && afterSecond == 1)) fails++;
    // patch round trip keeps the notes
    m.inputs[MintySeq::CLOCK_INPUT].channels = 0;
    m.seq.currentPattern().writeNote(10, 3, 1.5f);
    json_t* j = m.dataToJson();
    MintySeq m2;
    m2.dataFromJson(j);
    json_decref(j);
    const minty::Pattern& p2 = m2.seq.currentPattern();
    bool roundTrip = p2.steps[10].gate && p2.steps[10].cv == 1.5f && p2.steps[12].tie && !p2.steps[13].tie;
    std::printf("minty: json round trip %d\n", roundTrip);
    if (!roundTrip) fails++;
    return fails;
}

int main() {
    int fails = smokeGChip() + smokeMintySeq();
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
    // the step buttons: a press is one step, wrapping around at both ends
    {
        Module::ProcessArgs a;
        a.sampleRate = 48000.f;
        a.sampleTime = 1.f / 48000.f;
        auto press = [&](int param) {
            m.params[param].setValue(1.f);
            m.process(a);
            m.process(a);  // held: no second step
            m.params[param].setValue(0.f);
            m.process(a);
        };
        m.params[HChipFilter::TYPE_PARAM].setValue(0);
        press(HChipFilter::TYPE_PREV_PARAM);
        int t = (int)m.params[HChipFilter::TYPE_PARAM].getValue();
        std::printf("prev from 0: type %d\n", t);
        if (t != kNumTypes - 1) fails++;
        press(HChipFilter::TYPE_NEXT_PARAM);
        t = (int)m.params[HChipFilter::TYPE_PARAM].getValue();
        std::printf("next from %d: type %d\n", kNumTypes - 1, t);
        if (t != 0) fails++;
        press(HChipFilter::TYPE_NEXT_PARAM);
        t = (int)m.params[HChipFilter::TYPE_PARAM].getValue();
        if (t != 1) fails++;
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
