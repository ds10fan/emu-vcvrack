#include "wav.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace wav {
namespace {

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

int16_t toWord(double v) {
    double s = std::round(v * 32768.0);
    if (s > 32767.0) s = 32767.0;
    if (s < -32768.0) s = -32768.0;
    return int16_t(s);
}

// One sample frame's channel `ch` as a double in [-1, 1).
double readSample(const uint8_t* p, int bits, bool isFloat) {
    if (isFloat) {
        if (bits == 32) {
            float f;
            std::memcpy(&f, p, 4);
            return f;
        }
        double d;
        std::memcpy(&d, p, 8);
        return d;
    }
    switch (bits) {
        case 8: return (int(p[0]) - 128) / 128.0;
        case 16: return int16_t(u16(p)) / 32768.0;
        case 24: {
            int32_t v = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8;
            return v / 8388608.0;
        }
        case 32: return int32_t(u32(p)) / 2147483648.0;
        default: return 0.0;
    }
}

}  // namespace

bool parse(const uint8_t* b, size_t size, File& out, std::string& error) {
    out = File();
    if (size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    bool haveFmt = false;
    uint16_t format = 0;
    int channels = 0, bits = 0, blockAlign = 0;
    double rate = 0;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* id = b + pos;
        size_t len = u32(b + pos + 4);
        const uint8_t* body = b + pos + 8;
        if (len > size - pos - 8) len = size - pos - 8;  // truncated file: use what is there
        if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            blockAlign = u16(body + 12);
            bits = u16(body + 14);
            if (format == 0xFFFE && len >= 40) format = u16(body + 24);  // extensible: sub-format GUID's first word
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = body;
            dataSize = len;
        } else if (std::memcmp(id, "smpl", 4) == 0 && len >= 36) {
            uint32_t nloops = u32(body + 28);
            if (nloops >= 1 && len >= 36 + 24) {
                const uint8_t* lp = body + 36;
                uint32_t start = u32(lp + 8), end = u32(lp + 12);
                out.hasLoop = true;
                out.loopStart = start;
                out.loopEnd = int64_t(end) + 1;
            }
        }
        pos += 8 + len + (len & 1);
    }
    if (!haveFmt) {
        error = "no fmt chunk";
        return false;
    }
    if (!data) {
        error = "no data chunk";
        return false;
    }
    bool isFloat = format == 3;
    if (!(format == 1 || format == 3)) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "unsupported format tag 0x%04X", format);
        error = buf;
        return false;
    }
    if (channels < 1 || rate <= 0) {
        error = "bad channel count or sample rate";
        return false;
    }
    if (!((isFloat && (bits == 32 || bits == 64)) || (!isFloat && (bits == 8 || bits == 16 || bits == 24 || bits == 32)))) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "unsupported %d-bit %s", bits, isFloat ? "float" : "PCM");
        error = buf;
        return false;
    }
    const int bytesPerSample = bits / 8;
    if (blockAlign <= 0) blockAlign = bytesPerSample * channels;
    const size_t frames = dataSize / size_t(blockAlign);
    out.rate = rate;
    out.channels = channels;
    out.bitsPerSample = bits;
    out.isFloat = isFloat;
    out.mono.resize(frames);
    for (size_t f = 0; f < frames; f++) {
        const uint8_t* p = data + f * size_t(blockAlign);
        double acc = 0.0;
        for (int ch = 0; ch < channels; ch++) acc += readSample(p + ch * bytesPerSample, bits, isFloat);
        out.mono[f] = toWord(acc / channels);
    }
    if (out.hasLoop && (out.loopEnd > int64_t(frames) || out.loopStart >= out.loopEnd)) out.hasLoop = false;
    return true;
}

bool load(const std::string& path, File& out, std::string& error) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        error = "cannot open file";
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(fp);
    return parse(bytes.data(), bytes.size(), out, error);
}

}  // namespace wav
