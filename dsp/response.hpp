// Frequency response of a Filter Lab program: an impulse into IN1 of a fresh voice, through the
// oversampler as in the module, then the magnitude spectrum of each output.
#pragma once

#include <cmath>
#include <vector>

#include "fscript.hpp"
#include "oversampler.hpp"

namespace filterlab {

// In-place radix-2 FFT; the size must be a power of two.
inline void fft(std::vector<double>& re, std::vector<double>& im) {
    const int n = (int)re.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                const int a = i + k, b = a + len / 2;
                const double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const double t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

struct Response {
    static const int kOuts = 4;
    int n = 0;                     // impulse response length, samples at the host rate
    double sampleRate = 0;         // host rate; bin k is k * sampleRate / n
    std::vector<float> mag[kOuts]; // n / 2 + 1 linear gains per output
    bool active[kOuts] = {};       // the output is not silent
    bool ok = false;               // false: the voice produced NaN/infinity or ran out of loop budget

    // Gain of an output at a frequency (linear interpolation between bins).
    float at(int out, double hz) const {
        if (n == 0) return 0.f;
        double b = hz * n / sampleRate;
        if (b <= 0) return mag[out][0];
        if (b >= n / 2) return mag[out][n / 2];
        int i = (int)b;
        float f = (float)(b - i);
        return mag[out][i] + (mag[out][i + 1] - mag[out][i]) * f;
    }
};

// p: p1..p4 (0..1), cv: cv1..cv4 (volts), level: the impulse's height (the gains are relative to it).
// The last quarter of the impulse response is faded out, so a response still ringing at the end
// is smoothed rather than truncated.
inline bool analyse(const fscript::Program& prog, const double* p, const double* cv, double hostRate, int os,
                    double level, int n, Response& r) {
    r.n = n;
    r.sampleRate = hostRate;
    r.ok = false;
    for (int i = 0; i < Response::kOuts; i++) {
        r.mag[i].assign(n / 2 + 1, 0.f);
        r.active[i] = false;
    }

    fscript::Voice v;
    fscript::prepare(prog, v);
    auto setInputs = [&]() {
        for (int i = 0; i < 4; i++) {
            v.mem[fscript::B_P1 + i] = p[i];
            v.mem[fscript::B_CV1 + i] = cv[i];
        }
    };
    setInputs();
    if (!fscript::reset(prog, v, hostRate * os, 0)) return false;

    Oversampler osIn, osOut[Response::kOuts];
    osIn.setFactor(os);
    for (Oversampler& o : osOut) o.setFactor(os);
    os = osIn.factor;

    std::vector<double> t[Response::kOuts];
    for (std::vector<double>& x : t) x.assign(n, 0.0);
    float up[Oversampler::kMaxFactor], y[Response::kOuts][Oversampler::kMaxFactor];
    for (int s = 0; s < n; s++) {
        osIn.up(s == 0 ? (float)level : 0.f, up);
        for (int k = 0; k < os; k++) {
            setInputs();
            v.mem[fscript::B_IN] = up[k];
            v.mem[fscript::B_IN2] = v.mem[fscript::B_IN3] = v.mem[fscript::B_IN4] = 0.0;
            if (!fscript::run(prog, v)) return false;
            for (int i = 0; i < Response::kOuts; i++) {
                if (!std::isfinite(v.mem[fscript::B_OUT + i])) return false;
                y[i][k] = (float)v.mem[fscript::B_OUT + i];
            }
        }
        for (int i = 0; i < Response::kOuts; i++) {
            float o = osOut[i].down(y[i]);
            if (!std::isfinite(o)) return false;
            t[i][s] = o / level;
        }
    }

    std::vector<double> im;
    const int fade = n / 4;
    for (int i = 0; i < Response::kOuts; i++) {
        double peak = 0;
        for (int s = 0; s < n; s++) peak = std::fmax(peak, std::fabs(t[i][s]));
        r.active[i] = peak > 1e-9;
        if (!r.active[i]) continue;
        for (int s = n - fade; s < n; s++) t[i][s] *= 0.5 + 0.5 * std::cos(M_PI * (s - (n - fade)) / fade);
        im.assign(n, 0.0);
        fft(t[i], im);
        for (int k = 0; k <= n / 2; k++) r.mag[i][k] = (float)std::sqrt(t[i][k] * t[i][k] + im[k] * im[k]);
    }
    r.ok = true;
    return true;
}

}  // namespace filterlab
