// minty-seq core: a 7-track CV/gate step recorder. Ten sets of seven tracks, each track two
// patterns (A/B) of 64 sixteenth steps (4 bars of 4/4). Notes are recorded quantised to the
// nearest sixteenth with their length as ties. Clocked by an internal step rate or by external
// pulses, one per sixteenth. No Rack dependency; the module adds the I/O, the gate gaps and the
// monitoring.
#pragma once
#include <cstdint>

namespace minty {

constexpr int kTracks = 7;
constexpr int kSets = 10;
constexpr int kPatterns = 2;
constexpr int kSteps = 64;  // 4 bars x 16

struct Step {
    float cv = 0.f;
    bool gate = false;  // a note starts here
    bool tie = false;   // the previous step's note continues through this one
};

struct Pattern {
    Step steps[kSteps];
    void clear() {
        for (auto& s : steps) s = Step();
    }
    bool empty() const {
        for (const auto& s : steps)
            if (s.gate || s.tie) return false;
        return true;
    }
    // Write a note of `len` steps starting at `start`, replacing whatever it covers and cutting
    // the tail of any note that continued past its end.
    void writeNote(int start, int len, float cv) {
        if (len < 1) len = 1;
        if (len > kSteps) len = kSteps;
        for (int k = 0; k < len; k++) {
            Step& s = steps[(start + k) % kSteps];
            s.cv = cv;
            s.gate = k == 0;
            s.tie = k > 0;
        }
        for (int k = len; k < kSteps; k++) {
            Step& s = steps[(start + k) % kSteps];
            if (!s.tie) break;
            s.tie = false;
        }
    }
};

struct Track {
    Pattern pattern[kPatterns];
};

struct Set {
    Track tracks[kTracks];
    int ab[kTracks] = {0, 0, 0, 0, 0, 0, 0};  // which pattern each track plays
};

class Sequencer {
public:
    Set sets[kSets];
    int currentSet = 0;
    int currentTrack = 0;
    bool running = false;
    bool recording = false;

    // Transport position: the step being played and the fraction of it elapsed (0..1).
    int pos() const { return pos_; }
    double phase() const { return phase_; }
    int bar() const { return pos_ / 16; }
    int beat() const { return (pos_ % 16) / 4; }

    // Per-track playback state after the last step.
    float trackCv(int t) const { return cv_[t]; }
    bool trackGate(int t) const { return gate_[t]; }
    // True once per step boundary (including step 0 at start), with the step that just began.
    bool takeStepEvent(int& step) {
        if (!stepEvent_) return false;
        stepEvent_ = false;
        step = pos_;
        return true;
    }
    // Set at a step where a new note starts while the gate was already high; the caller clears
    // it after producing the gap.
    bool takeRetrigger(int t) {
        bool r = retrig_[t];
        retrig_[t] = false;
        return r;
    }

    // ---- transport ----
    void start() {
        running = true;
        reset();
    }
    void stop() {
        if (recording) noteOff();
        running = false;
        recording = false;
        for (int t = 0; t < kTracks; t++) gate_[t] = false;
    }
    void toggleRun() {
        if (running) stop();
        else start();
    }
    // Record arms the current track; if stopped it also starts the transport.
    void toggleRecord() {
        if (recording) {
            noteOff();
            recording = false;
        } else {
            recording = true;
            if (!running) start();
        }
    }
    // Back to step 0: the next clock plays it.
    void reset() {
        pos_ = 0;
        phase_ = 0.0;
        pendingFirst_ = true;
        extElapsed_ = 0.0;
    }

    // ---- selection ----
    void selectTrack(int t) {
        if (t >= 0 && t < kTracks && t != currentTrack) {
            if (recActive_) noteOff();
            currentTrack = t;
        }
    }
    void selectSet(int s) {
        if (s >= 0 && s < kSets && s != currentSet) {
            if (recActive_) noteOff();
            currentSet = s;
        }
    }
    void toggleAB(int t) { setAB(t, 1 - sets[currentSet].ab[t]); }
    void setAB(int t, int p) {
        if (t >= 0 && t < kTracks && p >= 0 && p < kPatterns) {
            if (t == currentTrack && recActive_) noteOff();
            sets[currentSet].ab[t] = p;
        }
    }
    int ab(int t) const { return sets[currentSet].ab[t]; }
    Pattern& pattern(int set, int t, int p) { return sets[set].tracks[t].pattern[p]; }
    Pattern& currentPattern() { return sets[currentSet].tracks[currentTrack].pattern[sets[currentSet].ab[currentTrack]]; }
    const Pattern& playingPattern(int t) const { return sets[currentSet].tracks[t].pattern[sets[currentSet].ab[t]]; }
    void clearCurrent() {
        if (recActive_) recActive_ = false;
        currentPattern().clear();
        gate_[currentTrack] = false;
    }
    void clearAll() {
        for (auto& s : sets) s = Set();
        recActive_ = false;
        for (int t = 0; t < kTracks; t++) gate_[t] = false;
    }

    // ---- clock: call one of these every sample ----
    // Internal: `stepsPerSample` = bpm / 60 * 4 / sampleRate.
    void tickInternal(double stepsPerSample) {
        if (!running) return;
        if (pendingFirst_) {
            pendingFirst_ = false;
            step(true);
        }
        phase_ += stepsPerSample;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            step(false);
        }
    }
    // External: `pulse` is true on the sample a clock edge arrives; the period is measured so the
    // recording can still quantise to the nearest step. `fallbackPeriod` is used before two pulses.
    void tickExternal(bool pulse, double sampleTime, double fallbackPeriod) {
        if (!running) return;
        extElapsed_ += sampleTime;
        if (pulse) {
            if (extElapsed_ > 1e-3 && !pendingFirst_) extPeriod_ = extElapsed_;
            extElapsed_ = 0.0;
            if (pendingFirst_) {
                pendingFirst_ = false;
                step(true);
            } else {
                step(false);
            }
        }
        double period = extPeriod_ > 0.0 ? extPeriod_ : fallbackPeriod;
        phase_ = period > 0.0 ? extElapsed_ / period : 0.0;
        if (phase_ > 0.999) phase_ = 0.999;
    }
    double externalPeriod() const { return extPeriod_; }

    // ---- recording ----
    // A key went down with this CV. Quantised to the nearest step: the current one if less than
    // half of it has passed, else the next.
    void noteOn(float cv) {
        if (!recording) return;
        if (recActive_) noteOff();
        recStart_ = nearestStep();
        recCv_ = cv;
        recSteps_ = 0;
        recActive_ = true;
    }
    // The key came up: write the note with its quantised length (at least one step).
    void noteOff() {
        if (!recActive_) return;
        recActive_ = false;
        int end = nearestStep();
        int len = (end - recStart_ + kSteps) % kSteps;
        if (recSteps_ >= kSteps) len = kSteps;
        if (len < 1) len = 1;
        Pattern& p = currentPattern();
        p.writeNote(recStart_, len, recCv_);
        // If the note's first step is already past, the playback state would not have seen it:
        // sound it from here on so the recorded and the heard note agree.
        int rel = (pos_ - recStart_ + kSteps) % kSteps;
        if (rel < len) {
            cv_[currentTrack] = recCv_;
            gate_[currentTrack] = true;
        }
    }
    bool noteHeld() const { return recActive_; }

private:
    int nearestStep() const {
        if (pendingFirst_) return 0;
        return phase_ < 0.5 ? pos_ : (pos_ + 1) % kSteps;
    }

    void step(bool first) {
        if (!first) {
            pos_ = (pos_ + 1) % kSteps;
            if (recActive_) recSteps_++;
        }
        stepEvent_ = true;
        for (int t = 0; t < kTracks; t++) {
            const Step& s = playingPattern(t).steps[pos_];
            if (s.gate) {
                if (gate_[t]) retrig_[t] = true;
                cv_[t] = s.cv;
                gate_[t] = true;
            } else if (s.tie) {
                // hold
            } else {
                gate_[t] = false;
            }
        }
    }

    int pos_ = 0;
    double phase_ = 0.0;
    bool pendingFirst_ = true;
    bool stepEvent_ = false;
    double extElapsed_ = 0.0, extPeriod_ = 0.0;
    float cv_[kTracks] = {0, 0, 0, 0, 0, 0, 0};
    bool gate_[kTracks] = {false, false, false, false, false, false, false};
    bool retrig_[kTracks] = {false, false, false, false, false, false, false};
    bool recActive_ = false;
    int recStart_ = 0, recSteps_ = 0;
    float recCv_ = 0.f;
};

}  // namespace minty
