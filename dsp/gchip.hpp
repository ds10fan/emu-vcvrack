// The E-mu G-chip sample interpolator of US 5,111,727 as specified in
// the interpolator specification: one 223-point linear-phase FIR prototype at 32
// sub-phases, seven taps per output sample, coefficients read from the symmetric half (112 words
// plus a zero at address 112) at |(-80 + 32 n) - f_MS| and blended toward the next address with
// the fine phase. Plus a playback voice: phase accumulator with 32 fraction bits, loop handling
// with wrapped reads (equivalent to replicated guard samples). No Rack dependency.
#pragma once
#include <cstdint>
#include <vector>

namespace gchip {

constexpr int kTaps = 7;
constexpr int kPhases = 32;
constexpr int kRomWords = 112;
constexpr int kFracBits = 32;          // phase accumulator fraction bits
constexpr int kBlendBits = 8;          // f_LS bits reaching the blend multiplier (fixed-point path)
constexpr int kCoefUnityBits = 14;     // fixed-point coefficient unity (peak 1.0338 fits 16 bits)

// Prototype and ROM, exact (double) and fixed point.
double proto(int index);                       // 0..222
double rom(int addr);                          // 0..111 from the prototype, 0 at 112 and beyond
double norm();                                 // 32 / sum(PROTO)
int32_t romFixed(int addr);                    // round(rom(addr) * norm() * 2^kCoefUnityBits)

// Seven coefficients for sub-phase fMs (0..31) and fine phase fLs in [0, 1), normalised to sum 1.
void coeffs(int fMs, double fLs, double c[kTaps]);
// The fixed-point form of the spec's scalar C: fLs8 is the top 8 fine bits, coefficients at
// unity 2^kCoefUnityBits.
void coeffsFixed(int fMs, uint32_t fLs8, int32_t c[kTaps]);

// Split a Q32 fraction into the sub-phase and the fine remainder.
inline int phaseMs(uint32_t frac) { return int(frac >> (kFracBits - 5)); }
inline double phaseLs(uint32_t frac) {
    return double(frac & ((uint32_t(1) << (kFracBits - 5)) - 1)) / double(uint32_t(1) << (kFracBits - 5));
}
inline uint32_t phaseLs8(uint32_t frac) { return (frac >> (kFracBits - 5 - kBlendBits)) & 0xFF; }

// One output sample from the window x[i-2 .. i+4] supplied by `fetch(j)`.
template <typename Fetch>
double interpolate(const Fetch& fetch, int64_t i, uint32_t frac) {
    double c[kTaps];
    coeffs(phaseMs(frac), phaseLs(frac), c);
    double acc = 0.0;
    for (int n = 0; n < kTaps; n++) acc += c[n] * double(fetch(i + n - 2));
    return acc;
}
template <typename Fetch>
int32_t interpolateFixed(const Fetch& fetch, int64_t i, uint32_t frac) {
    int32_t c[kTaps];
    coeffsFixed(phaseMs(frac), phaseLs8(frac), c);
    int64_t acc = 0;
    for (int n = 0; n < kTaps; n++) acc += int64_t(c[n]) * int64_t(fetch(i + n - 2));
    return int32_t(acc >> kCoefUnityBits);  // arithmetic shift: floor
}

// The spec's resample() of section 6: output n reads position n * ratio + 9. For the test vector.
void resampleReference(const std::vector<double>& x, double ratio, int nout, std::vector<double>& y);

// A loaded sample: 16-bit mono words, as the chip's waveform memory holds them.
struct Sample {
    std::vector<int16_t> data;
    double rate = 48000.0;
    bool hasLoop = false;
    int64_t loopStart = 0, loopEnd = 0;  // from the file, if any; end exclusive
};

enum class LoopMode { Off, Gate, On };

// One playback voice.
class Voice {
public:
    // Start at `start` (sample index). The loop [loopStart, loopEnd) is used when `mode` is not
    // Off and it is at least 3 samples long and lies inside the sample.
    void trigger(const Sample* s, int64_t start, int64_t loopStart, int64_t loopEnd, LoopMode mode);
    // Gate released: in LoopMode::Gate the voice leaves the loop at its next end and plays on;
    // in the other modes nothing happens.
    void release();
    void stop() { active_ = false; }
    bool active() const { return active_; }
    // Playback rate as a Q32 phase increment (1.0 = 1 << 32).
    void setIncrement(uint64_t inc) { inc_ = inc; }
    // One output sample in 16-bit units (double path or fixed-point path). Sets `ended` when the
    // voice has just run off the end of the sample.
    double process(bool fixedPoint, bool& ended);
    // Current integer position (for a display), or -1.
    int64_t position() const { return active_ ? int64_t(addr_ >> kFracBits) : -1; }

private:
    int16_t fetch(int64_t j) const;
    const Sample* s_ = nullptr;
    bool active_ = false;
    LoopMode mode_ = LoopMode::Off;
    bool looping_ = false;
    bool wrapped_ = false;
    uint64_t addr_ = 0;
    uint64_t inc_ = uint64_t(1) << kFracBits;
    int64_t loopStart_ = 0, loopEnd_ = 0;
};

}  // namespace gchip
