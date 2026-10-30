// E-mu Emulator IV filter recipes: the firmware's computeFrames for the 21 stock filters (EOS 3.20e,
// ghidra/out/filters.c) and the 13 filters of the OBXa patch (crates/eos320/src/reference.rs), turning
// (filter, Q, parameters) into the ten 32-bit words of the voice's H-chip block, the gain-compensation
// return value and the chip mode flags. No Rack dependency.
#pragma once
#include <cstdint>
#include <string>

namespace emuz {

constexpr int kNumFilters = 34;
constexpr uint32_t kNeutral = 0xFFCFFFCFu;
// DAT_00f01d96: the gain base the firmware writes into word 9 (.data initial value of EOS 3.20e).
constexpr uint32_t kGainBase = 0x00FE00FEu;

// Display law of parameter 0 (E-mu's fil_freq: maxfreq * (mul/1024)^(255 - v), integer arithmetic).
enum class FreqLaw { Lowpass, Highpass, Bandpass, Shelf, Raw };

struct FilterDesc {
    const char* name;      // the E4 menu name
    uint8_t type;          // the type ID stored in presets
    uint8_t nparams;       // 2, or 8 for the morph filters
    FreqLaw law;           // how parameter 0 is displayed
    const char* label0;    // parameter 0 label ("Frequency", "Morph", "Fc / Morph")
    const char* label1;    // parameter 1 label ("Q", "Gain", "Resonance", "Body Size", "Fuzz", ...)
    bool dbDisplay1;       // parameter 1 is displayed as (v - 64) * 0.375 dB
};

// One voice block as computeFrames leaves it.
struct Block {
    uint32_t w[10];        // words 0..9; words 2, 3, 6, 7 are never written (left at `fill`)
    int32_t ret;           // gain-compensation return value (voice gain units, see hchip.hpp)
    uint8_t flags;         // flags of the last hchip_setMode call: 0x40 lowpass (word 9 = gain, zeros
                           // inert), 0xC0 zeros on (word 1 pairs with 4, 5 with 8, word 9 = gain), 0x00 vocal
};

const FilterDesc& filterDesc(int menu);

// The firmware's computeFrames for menu entry `menu` (0..33), q 0..127 (masked to 7 bits), and the six
// bytes +6..+11 of the voice filter struct the morph filters read (ignored by the others).
Block computeFrames(int menu, int q, const uint8_t params[6], uint32_t fill = 0xA5A5A5A5u);

// E-mu's fil_freq display recurrence (FUN_00080396).
int filFreq(int v, int maxf, int mul);

// The firmware's formatParam: label and value text of parameter idx (0..7) at `value`.
void formatParam(int menu, int idx, int value, std::string& label, std::string& text);

// Label of one of the six extra parameters (idx 2..7) of a morph filter, "" for the others.
const char* extraParamLabel(int menu, int idx);

// Byte helpers for a stage word [X_lo F_lo | X_hi F_hi].
inline int xLo(uint32_t w) { return (w >> 24) & 0xFF; }
inline int fLo(uint32_t w) { return (w >> 16) & 0xFF; }
inline int xHi(uint32_t w) { return (w >> 8) & 0xFF; }
inline int fHi(uint32_t w) { return w & 0xFF; }

}  // namespace emuz
