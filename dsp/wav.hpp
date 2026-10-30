// A small RIFF/WAVE reader: PCM 8/16/24/32-bit and IEEE float 32/64-bit, any channel count
// (mixed down to mono), WAVE_FORMAT_EXTENSIBLE, and the first loop of a `smpl` chunk. Output is
// 16-bit words (rounded) as the chip's waveform memory holds them. No Rack dependency.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wav {

struct File {
    std::vector<int16_t> mono;   // samples, channels averaged
    double rate = 0.0;
    int channels = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    bool hasLoop = false;
    int64_t loopStart = 0, loopEnd = 0;  // end exclusive (the smpl chunk's end is inclusive)
};

// Parse from memory. Returns false and sets `error` on failure.
bool parse(const uint8_t* bytes, size_t size, File& out, std::string& error);
// Read a file from disk and parse it.
bool load(const std::string& path, File& out, std::string& error);

}  // namespace wav
