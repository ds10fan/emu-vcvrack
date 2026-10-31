// Polyphase windowed-sinc oversampler for Filter Lab (factor 1, 2, 4 or 8).
#pragma once

#include <cmath>
#include <vector>

namespace filterlab {

struct Oversampler {
    static const int kTapsPerPhase = 16;
    static const int kMaxFactor = 8;

    int factor = 1;
    std::vector<float> h;  // kTapsPerPhase * factor taps
    float upHist[kTapsPerPhase] = {};
    int upPos = 0;
    std::vector<float> downHist;  // 2 * taps, written twice so a window is always contiguous
    int downPos = 0;

    void setFactor(int f) {
        factor = f < 1 ? 1 : (f > kMaxFactor ? kMaxFactor : f);
        int L = kTapsPerPhase * factor;
        h.assign(L, 0.f);
        double fc = 0.45 / factor;  // cycles per oversampled sample
        double sum = 0;
        std::vector<double> t(L);
        for (int i = 0; i < L; i++) {
            double x = i - (L - 1) / 2.0;
            double sinc = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            double w = 0.42 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / L) + 0.08 * std::cos(4 * M_PI * (i + 0.5) / L);
            t[i] = sinc * w;
            sum += t[i];
        }
        for (int i = 0; i < L; i++) h[i] = (float)(t[i] / sum);
        downHist.assign(2 * L, 0.f);
        reset();
    }

    void reset() {
        for (float& x : upHist) x = 0;
        for (float& x : downHist) x = 0;
        upPos = downPos = 0;
    }

    // One input sample to `factor` output samples.
    void up(float x, float* out) {
        if (factor == 1) {
            out[0] = x;
            return;
        }
        upPos = (upPos + kTapsPerPhase - 1) % kTapsPerPhase;
        upHist[upPos] = x;
        for (int k = 0; k < factor; k++) {
            float s = 0;
            for (int j = 0; j < kTapsPerPhase; j++) s += h[j * factor + k] * upHist[(upPos + j) % kTapsPerPhase];
            out[k] = s * factor;
        }
    }

    // `factor` input samples to one output sample.
    float down(const float* in) {
        if (factor == 1) return in[0];
        int L = (int)h.size();
        for (int k = 0; k < factor; k++) {
            downPos = (downPos + L - 1) % L;
            downHist[downPos] = downHist[downPos + L] = in[k];
        }
        const float* w = &downHist[downPos];
        float s = 0;
        for (int i = 0; i < L; i++) s += h[i] * w[i];
        return s;
    }
};

}  // namespace filterlab
