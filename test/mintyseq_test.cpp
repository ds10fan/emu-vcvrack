// dsp/mintyseq.hpp: quantised recording, ties and overdubs, playback and retriggers, A/B and
// set switching, internal and external clocks.
#include <cmath>
#include <cstdio>

#include "../dsp/mintyseq.hpp"

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

// 127 BPM at 48 kHz: one sixteenth is 60 / 127 / 4 s = 5669.3 samples.
static const double kFs = 48000.0;
static const double kStepsPerSample = 127.0 / 60.0 * 4.0 / kFs;
static const int kStepSamples = int(kFs / (127.0 / 60.0 * 4.0));

static void runInternal(minty::Sequencer& q, int samples) {
    for (int i = 0; i < samples; i++) q.tickInternal(kStepsPerSample);
}

// Tick until the position has changed n times (the step is 5669.29 samples, so counting samples
// drifts); afterwards the phase is just past a step boundary.
static void advanceSteps(minty::Sequencer& q, int n) {
    int last = q.pos(), moved = 0;
    for (long i = 0; i < 100L * kStepSamples && moved < n; i++) {
        q.tickInternal(kStepsPerSample);
        if (q.pos() != last) {
            last = q.pos();
            moved++;
        }
    }
}

static void checkRecording() {
    minty::Sequencer q;
    q.toggleRecord();  // arms and starts
    CHECK(q.running && q.recording, "record starts the transport");
    runInternal(q, 1);  // step 0 fires immediately
    CHECK(q.pos() == 0, "first step is 0, got %d", q.pos());
    // A note 10 % into step 2, held for 2.3 steps: lands on step 2 with length 2.
    advanceSteps(q, 2);
    runInternal(q, kStepSamples / 10);
    CHECK(q.pos() == 2, "at step 2, got %d (phase %.2f)", q.pos(), q.phase());
    q.noteOn(1.25f);
    runInternal(q, int(2.3 * kStepSamples));
    q.noteOff();
    const minty::Pattern& p = q.currentPattern();
    CHECK(p.steps[2].gate && !p.steps[2].tie && p.steps[2].cv == 1.25f, "note at step 2");
    CHECK(!p.steps[3].gate && p.steps[3].tie, "tie at step 3");
    CHECK(!p.steps[4].gate && !p.steps[4].tie, "step 4 free");
    // A late note (70 % into step 6) goes to step 7; very short: one step.
    advanceSteps(q, 2);
    CHECK(q.pos() == 6, "at step 6, got %d", q.pos());
    runInternal(q, int(0.7 * kStepSamples));  // 70 % into step 6
    q.noteOn(-0.5f);
    runInternal(q, kStepSamples / 20);
    q.noteOff();
    CHECK(p.steps[7].gate && p.steps[7].cv == -0.5f && !p.steps[8].tie, "late short note on step 7");
    CHECK(!p.steps[6].gate, "step 6 stays free");
    // Overdub: a long note from step 1 covering 2..3 replaces the first note and keeps no tail.
    q.stop();
    q.start();
    q.recording = true;
    runInternal(q, 1);
    advanceSteps(q, 1);  // early in step 1
    q.noteOn(2.0f);
    advanceSteps(q, 3);  // release early in step 4 -> rounds to 4: length 3
    q.noteOff();
    CHECK(p.steps[1].gate && p.steps[1].cv == 2.0f, "overdub starts at 1");
    CHECK(p.steps[2].tie && !p.steps[2].gate && p.steps[2].cv == 2.0f, "step 2 now a tie of the new note");
    CHECK(p.steps[3].tie && !p.steps[4].tie && !p.steps[4].gate, "ends at 3, no tail");
    CHECK(p.steps[7].gate, "the other note is untouched");
    // Note held longer than the pattern: full length.
    q.clearCurrent();
    CHECK(p.empty(), "cleared");
    runInternal(q, 5);
    q.noteOn(0.f);
    advanceSteps(q, 70);
    q.noteOff();
    int ties = 0, gates = 0;
    for (const auto& s : p.steps) {
        ties += s.tie;
        gates += s.gate;
    }
    CHECK(gates == 1 && ties == 63, "held across the loop: %d gates %d ties", gates, ties);
}

static void checkPlayback() {
    minty::Sequencer q;
    minty::Pattern& p = q.currentPattern();
    p.writeNote(0, 2, 1.f);  // steps 0-1
    p.writeNote(2, 1, 2.f);  // step 2 right after: retrigger
    p.writeNote(5, 1, 3.f);
    q.start();
    // step 0
    runInternal(q, 1);
    CHECK(q.trackGate(0) && q.trackCv(0) == 1.f && !q.takeRetrigger(0), "step 0 note on");
    advanceSteps(q, 1);
    CHECK(q.pos() == 1 && q.trackGate(0) && q.trackCv(0) == 1.f, "step 1 tied");
    advanceSteps(q, 1);
    CHECK(q.pos() == 2 && q.trackGate(0) && q.trackCv(0) == 2.f && q.takeRetrigger(0), "step 2 retriggers");
    advanceSteps(q, 1);
    CHECK(q.pos() == 3 && !q.trackGate(0), "step 3 silent");
    advanceSteps(q, 2);
    CHECK(q.pos() == 5 && q.trackGate(0) && q.trackCv(0) == 3.f && !q.takeRetrigger(0), "step 5 from silence: no retrigger");
    // other tracks silent
    for (int t = 1; t < minty::kTracks; t++) CHECK(!q.trackGate(t), "track %d silent", t);
    // wraps after 64 steps
    advanceSteps(q, 59);
    CHECK(q.pos() == 0 && q.trackGate(0) && q.trackCv(0) == 1.f, "wrapped to step 0 (pos %d)", q.pos());
    // A/B: pattern B is empty; switching silences the track at its next step; switching back plays.
    q.toggleAB(0);
    CHECK(q.ab(0) == 1, "B selected");
    advanceSteps(q, 1);
    CHECK(!q.trackGate(0), "B is empty");
    q.setAB(0, 0);
    advanceSteps(q, 1);  // step 2
    CHECK(q.trackGate(0) && q.trackCv(0) == 2.f, "back on A at step 2");
    // sets are independent
    q.selectSet(3);
    advanceSteps(q, 3);  // step 5
    CHECK(!q.trackGate(0), "set 3 is empty");
    q.selectSet(0);
    q.stop();
    CHECK(!q.running && !q.trackGate(0), "stopped: gates off");
    q.start();
    runInternal(q, 1);
    CHECK(q.pos() == 0 && q.trackGate(0), "start plays step 0 again");
}

static void checkExternalClock() {
    minty::Sequencer q;
    q.currentPattern().writeNote(1, 1, 1.f);
    q.start();
    const double dt = 1.0 / kFs;
    const int period = 6000;  // samples per pulse
    // no step until the first pulse
    for (int i = 0; i < 100; i++) q.tickExternal(false, dt, 0.1);
    CHECK(q.pos() == 0 && !q.trackGate(0), "waits for the first pulse");
    auto pulse = [&]() {
        q.tickExternal(true, dt, 0.1);
        for (int i = 1; i < period; i++) q.tickExternal(false, dt, 0.1);
    };
    pulse();  // step 0 played, now 1 period elapsed
    CHECK(q.pos() == 0, "first pulse is step 0");
    pulse();
    CHECK(q.pos() == 1 && q.trackGate(0) && q.trackCv(0) == 1.f, "second pulse is step 1");
    CHECK(std::fabs(q.externalPeriod() - period * dt) < 1e-6, "period measured %.5f", q.externalPeriod());
    // quantisation uses the measured period: a note 80 % into step 1 lands on step 2
    q.tickExternal(true, dt, 0.1);  // step 2 ... wait: this pulse makes pos 2
    CHECK(q.pos() == 2, "third pulse is step 2");
    q.recording = true;
    for (int i = 1; i < int(0.8 * period); i++) q.tickExternal(false, dt, 0.1);
    q.noteOn(5.f);
    for (int i = 0; i < 3 * period; i++) q.tickExternal(i % period == int(0.2 * period), dt, 0.1);
    q.noteOff();
    const minty::Pattern& p = q.currentPattern();
    CHECK(p.steps[3].gate && p.steps[3].cv == 5.f, "late note quantised to the next step (3)");
    // reset: the next pulse is step 0
    q.reset();
    q.tickExternal(true, dt, 0.1);
    CHECK(q.pos() == 0, "reset then pulse: step 0, got %d", q.pos());
}

int main() {
    checkRecording();
    checkPlayback();
    checkExternalClock();
    if (fails) {
        std::fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    std::printf("mintyseq test passed\n");
    return 0;
}
