#include "gchip.hpp"

#include <cmath>

namespace gchip {

#include "gchip_proto.inc"

namespace {
constexpr int kCentre = 111;

struct Tables {
    double norm;
    double rom[kRomWords + 1];
    int32_t romFixed[kRomWords + 1];
    Tables() {
        double sum = 0.0;
        for (int i = 0; i < 223; i++) sum += kProto[i];
        norm = double(kPhases) / sum;
        for (int a = 0; a < kRomWords; a++) {
            rom[a] = kProto[kCentre + a];
            romFixed[a] = int32_t(std::lround(rom[a] * norm * double(1 << kCoefUnityBits)));
        }
        rom[kRomWords] = 0.0;  // address 112: referenced by tap 6 at phase 0, reads zero
        romFixed[kRomWords] = 0;
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

inline int absi(int v) { return v < 0 ? -v : v; }
}  // namespace

double proto(int index) { return (index >= 0 && index < 223) ? kProto[index] : 0.0; }
double rom(int addr) { return (addr >= 0 && addr <= kRomWords) ? tables().rom[addr] : 0.0; }
double norm() { return tables().norm; }
int32_t romFixed(int addr) { return (addr >= 0 && addr <= kRomWords) ? tables().romFixed[addr] : 0; }

void coeffs(int fMs, double fLs, double c[kTaps]) {
    const Tables& t = tables();
    for (int n = 0; n < kTaps; n++) {
        int off = (-80 + 32 * n) - fMs;
        int a0 = absi(off);
        int a1 = absi(off - 1);
        double c0 = a0 <= kRomWords ? t.rom[a0] : 0.0;
        double c1 = a1 <= kRomWords ? t.rom[a1] : 0.0;
        c[n] = (c0 + (c1 - c0) * fLs) * t.norm;
    }
}

void coeffsFixed(int fMs, uint32_t fLs8, int32_t c[kTaps]) {
    const Tables& t = tables();
    for (int n = 0; n < kTaps; n++) {
        int off = (-80 + 32 * n) - fMs;
        int a0 = absi(off);
        int a1 = absi(off - 1);
        int32_t c0 = a0 <= kRomWords ? t.romFixed[a0] : 0;
        int32_t c1 = a1 <= kRomWords ? t.romFixed[a1] : 0;
        c[n] = c0 + (((c1 - c0) * int32_t(fLs8)) >> kBlendBits);
    }
}

void resampleReference(const std::vector<double>& x, double ratio, int nout, std::vector<double>& y) {
    y.assign(nout, 0.0);
    for (int n = 0; n < nout; n++) {
        double pos = n * ratio + kTaps + 2;
        int64_t i = int64_t(std::floor(pos));
        double frac = pos - double(i);
        double xf = frac * kPhases;
        int fMs = int(std::floor(xf));
        double fLs = xf - fMs;
        double c[kTaps];
        coeffs(fMs, fLs, c);
        double acc = 0.0;
        for (int k = 0; k < kTaps; k++) {
            int64_t j = i - 2 + k;
            acc += c[k] * ((j >= 0 && j < int64_t(x.size())) ? x[j] : 0.0);
        }
        y[n] = acc;
    }
}

// ---------------------------------------------------------------- Voice
void Voice::trigger(const Sample* s, int64_t start, int64_t loopStart, int64_t loopEnd, LoopMode mode) {
    s_ = s;
    if (!s || s->data.empty()) {
        active_ = false;
        return;
    }
    const int64_t len = int64_t(s->data.size());
    if (start < 0) start = 0;
    if (start >= len) start = len - 1;
    addr_ = uint64_t(start) << kFracBits;
    // A loop is used when it lies inside the sample, is at least 3 samples long and the start
    // point has not already passed it (then the voice plays through to the end).
    bool valid = loopStart >= 0 && loopEnd <= len && loopEnd - loopStart >= 3 && start < loopEnd;
    mode_ = mode;
    looping_ = mode != LoopMode::Off && valid;
    loopStart_ = loopStart;
    loopEnd_ = loopEnd;
    wrapped_ = false;
    active_ = true;
}

void Voice::release() {
    if (mode_ == LoopMode::Gate) looping_ = false;
}

int16_t Voice::fetch(int64_t j) const {
    if (looping_) {
        const int64_t len = loopEnd_ - loopStart_;
        while (j >= loopEnd_) j -= len;
        if (wrapped_) {
            while (j < loopStart_) j += len;
        }
    }
    if (j < 0 || j >= int64_t(s_->data.size())) return 0;
    return s_->data[size_t(j)];
}

double Voice::process(bool fixedPoint, bool& ended) {
    ended = false;
    if (!active_) return 0.0;
    const int64_t i = int64_t(addr_ >> kFracBits);
    // The window spans x[i-2 .. i+4]; once i-2 is past the end every tap reads zero.
    if (i - 2 >= int64_t(s_->data.size())) {
        active_ = false;
        ended = true;
        return 0.0;
    }
    const uint32_t frac = uint32_t(addr_ & 0xFFFFFFFFu);
    auto fetchFn = [this](int64_t j) { return fetch(j); };
    double y = fixedPoint ? double(interpolateFixed(fetchFn, i, frac)) : interpolate(fetchFn, i, frac);
    addr_ += inc_;
    // Address wrap at the loop end, so that between samples the position is always inside the loop.
    if (looping_) {
        const int64_t len = loopEnd_ - loopStart_;
        while (int64_t(addr_ >> kFracBits) >= loopEnd_) {
            addr_ -= uint64_t(len) << kFracBits;
            wrapped_ = true;
        }
    }
    return y;
}

}  // namespace gchip
