// DS10 drive: a hard clipper whose drive also sets the dry/wet mix,
//   out = mix(in, clamp(in * drive * 4, -1, 1), drive),  mix(a, b, t) = a + (b - a) t.
// Input and output are in units of full scale (1.0); drive is 0..1. No Rack dependency.
#pragma once

namespace ds10 {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }

inline float oneSample(float input, float drive) {
    return mix(input, clampf(input * (drive * 4.f), -1.f, 1.f), drive);
}

}  // namespace ds10
