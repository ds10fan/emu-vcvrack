#include "filterlab_presets.hpp"

namespace filterlab {

const Preset kPresets[] = {

{"TPT SVF", R"(#name TPT SVF
#p1 Cutoff
#p2 Resonance
#p3 LP-BP-HP
#p4 Drive
#out Morph
#out2 Notch
// Zavalishin / Cytomic trapezoidal state variable filter.
// p1: cutoff (0.1 = one octave), p2: resonance, p3: morph LP > BP > HP.

g = warp(hz(p1))
k = 2 - 1.98 * p2              // 1/Q
a1 = 1 / (1 + g * (g + k))
a2 = g * a1
a3 = g * a2

x = tanh(in * (1 + p4 * 7)) / (1 + p4 * 3)
v3 = x - ic2
v1 = a1 * ic1 + a2 * v3
v2 = ic2 + a2 * ic1 + a3 * v3
ic1 = 2 * v1 - ic1
ic2 = 2 * v2 - ic2

lp = v2
bp = v1 * k                    // unity gain at the peak
hp = x - k * v1 - v2
out = p3 < 0.5 ? mix(lp, bp, p3 * 2) : mix(bp, hp, p3 * 2 - 1)
out2 = lp + hp
)"},

{"Moog Ladder", R"(#name Moog Ladder (Huovilainen)
#p1 Cutoff
#p2 Resonance
#p3 Drive
#out LP 24 dB
#out2 LP 12 dB
// Huovilainen's nonlinear ladder: four tanh stages, run twice per sample.

const os = 2
fc = min(hz(p1), 0.45 * sr) / (sr * os)
fcr = 1.873 * fc^3 + 0.4955 * fc^2 - 0.649 * fc + 0.9988
acr = -3.9364 * fc^2 + 1.8409 * fc + 0.9968
tune = 1 - exp(-tau * fc * fcr)
res = p2 * 1.05
drive = 1 + p3 * 6

for j in 0..os {
  x = tanh(in * drive - 4 * res * acr * d5)
  y1 = y1 + tune * (x - t1)
  t1 = tanh(y1)
  y2 = y2 + tune * (t1 - t2)
  t2 = tanh(y2)
  y3 = y3 + tune * (t2 - t3)
  t3 = tanh(y3)
  y4 = y4 + tune * (t3 - t4)
  t4 = tanh(y4)
  d5 = (y4 + d4) * 0.5
  d4 = y4
}
out = d5 * (1 + res) / sqrt(drive)
out2 = y2 / sqrt(drive)
)"},

{"ZDF Ladder", R"(#name ZDF Ladder
#p1 Cutoff
#p2 Resonance
#p3 Drive
#p4 Bass comp
#out LP 24 dB
#out2 BP
// Four trapezoidal one-poles with the feedback loop solved (zero delay),
// tanh at the loop input. Self-oscillates near full resonance.

g = warp(hz(p1))
G = g / (1 + g)
k = p2 * 4.2
drive = 1 + p3 * 5
b1 = s1 / (1 + g)
b2 = s2 / (1 + g)
b3 = s3 / (1 + g)
b4 = s4 / (1 + g)
S = G * G * G * b1 + G * G * b2 + G * b3 + b4
x = in * drive * (1 + p4 * k)
u = tanh((x - k * S) / (1 + k * G^4))

v = (u - s1) * G
y1 = v + s1
s1 = y1 + v
v = (y1 - s2) * G
y2 = v + s2
s2 = y2 + v
v = (y2 - s3) * G
y3 = v + s3
s3 = y3 + v
v = (y3 - s4) * G
y4 = v + s4
s4 = y4 + v

out = y4 / sqrt(drive)
out2 = (y2 - y4) * 2 / sqrt(drive)
)"},

{"Korg35", R"(#name Korg35 (MS-20) LP
#p1 Cutoff
#p2 Resonance
#p3 Drive
#out LP
#out2 Pre-LP
// Will Pirkle's virtual analog Korg35 lowpass: LP1 -> LP2 with an HP in the
// feedback path, saturating the loop.

g = warp(hz(p1))
G = g / (1 + g)
K = 0.01 + p2 * 1.98
drive = 1 + p3 * 4

// LP1
v = (in * drive - z1) * G
y1 = v + z1
z1 = y1 + v

// loop
a0 = 1 / (1 - K * G + K * G * G)
S = z2 * (K - K * G) / (1 + g) - z3 / (1 + g)
u = tanh(a0 * (y1 + S))

// LP2
v = (u - z2) * G
y = v + z2
z2 = y + v

// HP in the feedback, driven by K * y
v = (K * y - z3) * G
lp3 = v + z3
z3 = lp3 + v

out = y / sqrt(drive)
out2 = y1 / drive
)"},

{"Chamberlin SVF", R"(#name Chamberlin SVF
#p1 Cutoff
#p2 Resonance
#p3 Drive
#out LP
#out2 BP
// The classic digital state variable filter (Hal Chamberlin, 1985),
// four times per sample for stability up high, with a soft-limited band path.

const os = 4
q = 2 - 1.97 * p2
f = 2 * sin(pi * min(hz(p1), sr * 0.45) / (sr * os))
f = min(f, sqrt(q * q + 4) - q - 0.01)   // the stability limit of this structure
x = in * (1 + p3 * 4)
for j in 0..os {
  lp = lp + f * bp
  hp = x - lp - q * bp
  bp = bp + f * hp
  bp = bp / (1 + 0.05 * p3 * bp * bp)
}
out = lp / (1 + p3 * 2)
out2 = bp * q / (1 + p3 * 2)
)"},

{"RBJ Biquad", R"(#name RBJ Biquad
#p1 Cutoff
#p2 Q
#p3 Type
#p4 Gain
#out Filter
#out2 Input - filter
// Robert Bristow-Johnson's cookbook biquads, transposed direct form II.
// p3 picks: lowpass, highpass, bandpass, notch, peaking EQ (p4 = -18..+18 dB).

f = min(hz(p1), 0.49 * sr)
q = 0.5 + p2 * p2 * 20
mode = floor(clamp(p3, 0, 1) * 4.999)
w = tau * f / sr
cw = cos(w)
a = sin(w) / (2 * q)
A = sqrt(db((p4 - 0.5) * 36))
a0 = 1 + a
a1 = -2 * cw
a2 = 1 - a
if mode == 0 {
  b0 = (1 - cw) / 2
  b1 = 1 - cw
  b2 = b0
} else if mode == 1 {
  b0 = (1 + cw) / 2
  b1 = -1 - cw
  b2 = b0
} else if mode == 2 {
  b0 = a
  b1 = 0
  b2 = -a
} else if mode == 3 {
  b0 = 1
  b1 = a1
  b2 = 1
} else {
  b0 = 1 + a * A
  b1 = a1
  b2 = 1 - a * A
  a0 = 1 + a / A
  a2 = 1 - a / A
}
y = b0 * in / a0 + z1
z1 = (b1 * in - a1 * y) / a0 + z2
z2 = (b2 * in - a2 * y) / a0
out = y
out2 = in - y
)"},

{"Butterworth 24", R"(#name Butterworth 24 dB
#p1 Cutoff
#p2 LP / HP
#out Filter
#out2 Other
// Fourth-order Butterworth: two cascaded biquads with Q 0.5412 and 1.3066.
// p2 below 0.5: lowpass on OUT; above: highpass on OUT. OUT2 is the other one.

buf Q = [0.5412, 1.3066]
buf z1[4]
buf z2[4]
f = min(hz(p1), 0.49 * sr)
w = tau * f / sr
cw = cos(w)
lp = in
hp = in
for i in 0..2 {
  a = sin(w) / (2 * Q[i])
  a0 = 1 + a
  // lowpass section i
  y = (1 - cw) / 2 * lp / a0 + z1[i]
  z1[i] = ((1 - cw) * lp + 2 * cw * y) / a0 + z2[i]
  z2[i] = ((1 - cw) / 2 * lp - (1 - a) * y) / a0
  lp = y
  // highpass section i (state in slots 2, 3)
  y = (1 + cw) / 2 * hp / a0 + z1[i + 2]
  z1[i + 2] = (-(1 + cw) * hp + 2 * cw * y) / a0 + z2[i + 2]
  z2[i + 2] = ((1 + cw) / 2 * hp - (1 - a) * y) / a0
  hp = y
}
out = p2 < 0.5 ? lp : hp
out2 = p2 < 0.5 ? hp : lp
)"},

{"One-Pole", R"(#name One-Pole LP/HP
#p1 Cutoff
#out LP 6 dB
#out2 HP 6 dB
// The simplest trapezoidal filter. A good place to start.

g = warp(hz(p1))
v = (in - s) * g / (1 + g)
lp = v + s
s = lp + v
out = lp
out2 = in - lp
)"},

{"Comb", R"(#name Comb
#p1 Pitch
#p2 Feedback -/+
#p3 Damping
#p4 Mix
#out Mix
#out2 Wet
// Feedback comb with a damping lowpass in the loop. p1 sets the delay as a
// pitch (1/f). p2 below the middle is negative feedback (odd harmonics).

buf d[65536]
dl = clamp(sr / hz(p1), 2, 65000)
y = tap(d, w - dl)
lp = mix(y, lp, p3 * 0.95)
fb = (p2 * 2 - 1) * 0.995
d[w] = clip(in + lp * fb)
w = (w + 1) % 65536
out = mix(in, y, p4)
out2 = y
)"},

{"Formant", R"(#name Formant (vowels)
#p1 Vowel A-E-I-O-U
#p2 Q
#p3 Shift
#p4 Dry
#out Formants
#out2 F1 only
// Three parallel bandpass biquads tuned to vowel formants, morphing A > E > I > O > U.

buf F1 = [800, 350, 270, 450, 325]
buf F2 = [1150, 2000, 2140, 800, 700]
buf F3 = [2900, 2800, 2950, 2830, 2700]
buf amp = [1, 0.5, 0.3]
buf fq[3]
buf z1[3]
buf z2[3]

v = clamp(p1, 0, 1) * 4
i0 = floor(v)
fr = v - i0
i1 = min(i0 + 1, 4)
shift = 2 ^ ((p3 - 0.5) * 2)
fq[0] = mix(F1[i0], F1[i1], fr) * shift
fq[1] = mix(F2[i0], F2[i1], fr) * shift
fq[2] = mix(F3[i0], F3[i1], fr) * shift
q = 2 + p2 * 30

sum = 0
for j in 0..3 {
  w = tau * min(fq[j], 0.45 * sr) / sr
  a = sin(w) / (2 * q)
  cw = cos(w)
  y = a * in / (1 + a) + z1[j]
  z1[j] = (2 * cw * y) / (1 + a) + z2[j]
  z2[j] = (-a * in - (1 - a) * y) / (1 + a)
  sum = sum + y * amp[j]
  if j == 0 {
    out2 = y
  }
}
out = sum * 2 + in * p4
)"},

{"Phaser", R"(#name Allpass Phaser
#p1 Rate
#p2 Depth
#p3 Feedback
#p4 Center
#out Mix
#out2 Wet
// Six first-order allpasses swept by a sine LFO, with feedback.

const stages = 6
buf z[6]
rate = 0.02 * 500 ^ p1          // 0.02 .. 10 Hz
ph = fract(ph + rate * dt)
f = hz(p4 * 0.8 + 0.1) * 2 ^ (p2 * 2.5 * sin(tau * ph))
t = warp(f)
a = (t - 1) / (t + 1)
x = in + p3 * 0.9 * last
for i in 0..stages {
  y = a * x + z[i]
  z[i] = x - a * y
  x = y
}
last = x
out = (in + x) * 0.5
out2 = x
)"},

{"Template", R"(#name Template
#p1 Knob 1
#p2 Knob 2
#p3 Knob 3
#p4 Knob 4
#out Out
#out2 Out 2
// fscript quick reference
//
// Runs once per sample per polyphonic channel. Every variable keeps its value
// between samples (per channel) and starts at 0.
//
// inputs : in, in2 (audio, 5 V = 1.0)   p1..p4 (knob + CV, 0..1, smoothed)
//          cv1..cv4 (raw CV volts)   sr (sample rate)   dt (1/sr)   ch (channel)
// outputs: out, out2 (1.0 = 5 V, clamped at +-12 V)
// consts : pi, tau, e
// ops    : + - * / % ^  < <= > >= == !=  && || !  c ? a : b
//          = += -= *= /=
// control: if c { } else if c { } else { }
//          for i in 0..4 { }          (4 excluded)
//          init { }                   (runs on reset: load, sample rate change)
// decls  : buf d[4096]                (per-voice array, index wraps)
//          buf t = [1, 2, 3]          (table)
//          const n = 4
//          fn sat(x, k) = tanh(x * k) / k
// funcs  : sin cos tan asin acos atan sinh cosh tanh exp log log2 log10 sqrt
//          abs sign floor ceil round fract pow atan2 min max clamp mix step
//          hz(p)  20 Hz * 1000^p        warp(f)  tan(pi f / sr)
//          mtof(n)  vtof(v)  db(x)  clip(x)  soft(x)  noise()
//          tap(buf, pos) interpolated read   len(buf)
// header : #name, #p1..#p4, #in2, #out, #out2 label the panel
//
// Edits compile as you type; state with the same name is kept, so the sound
// doesn't drop out. Ctrl+Z undoes, Tab indents.

out = in
out2 = in2
)"},

};

const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

}  // namespace filterlab
