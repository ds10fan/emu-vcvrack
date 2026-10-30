// Cycle-level model of the IIR filter section of US 5,170,369 (E-mu "H-chip"): FIG. 6 datapath,
// FIG. 7 block floating point multiplier, FIG. 8 coefficient decoder, driven by the reservation
// table as microcode, and one E4 voice (three sections in series programmed from ten control words).
//
// A C++ port of a Rust cycle-level emulator of the patent (its bits, coef, mult, section,
// microcode and voice modules), bit-exact against it (test/hchip_test.cpp). The interpolator of FIGs. 9
// to 12 (the time behaviour of the interpolating variable x) is not ported: the voice takes x
// directly, so a continuous cutoff maps straight onto the 16-bit x the coefficient math unit uses.
// No Rack dependency.
#pragma once
#include <cstdint>

namespace hchip {

// ---------------------------------------------------------------- bits.rs
// Keep the low `bits` bits of v, sign-extended (a register that overflows silently).
inline int64_t wrap(int64_t v, unsigned bits) {
    unsigned s = 64 - bits;
    return static_cast<int64_t>(static_cast<uint64_t>(v) << s) >> s;
}
inline int64_t maxVal(unsigned bits) { return (int64_t(1) << (bits - 1)) - 1; }
inline int64_t minVal(unsigned bits) { return -(int64_t(1) << (bits - 1)); }
inline int64_t sat(int64_t v, unsigned bits) {
    int64_t lo = minVal(bits), hi = maxVal(bits);
    return v < lo ? lo : (v > hi ? hi : v);
}
// One's complement as a `bits`-wide word (the "bubble" outputs of latches 13, 20 and ACC 10).
inline int64_t notb(int64_t v, unsigned bits) { return wrap(~v, bits); }
inline bool overflows(int64_t v, unsigned bits) { return v < minVal(bits) || v > maxVal(bits); }

// ---------------------------------------------------------------- coef.rs
// Fixed-point scale of a linear coefficient: 1.0 == 1 << kFracBits.
constexpr unsigned kFracBits = 18;
// Codes that decode to exactly 1.0 as an X byte and as an F byte (the neutral word FFCF).
constexpr uint8_t kCodeOneX = 255;
constexpr uint8_t kCodeOneF = 207;

// How a code is scaled when expanded.
enum class Role {
    Second,  // X byte: B2' or A2', table / 2^18
    First,   // F byte: the difference added to the held second coefficient, table / 2^16
    Gain     // gain byte: table / 2^18, not added to anything
};
inline unsigned roleShift(Role r) { return r == Role::First ? 2 : 0; }

// A coefficient as the multiplier sees it: unsigned mantissa and signed exponent, value
// mant * 2^exp in units of 2^-kFracBits. A negative exponent means the mantissa carries bits
// below the coefficient unit (fraction bits of an interpolated code).
struct BlockFloat {
    uint32_t mant = 0;
    int32_t exp = 0;
    int64_t linear() const;  // whole units of 2^-kFracBits (floor)
    double toDouble() const; // 1.0 == coefficient one, fraction included
};

// Drop low mantissa bits until the mantissa fits mantBits.
BlockFloat normalised(uint64_t mant, int32_t exp, unsigned mantBits);

// The code table, as recovered by measurement on the chip. Unitless; see Role for the scaling.
int64_t decodeLinear(uint8_t code);
// The table for a code with 8 fraction bits (code * 256), as LOGIC 43 and BARREL SHIFTER 44
// produce it: mantissa = hidden bit, four mantissa bits and eight fraction bits; shift from the
// integer part. Value in table units = mantQ8 * 2^shift / 256.
void decodeQ8Parts(uint16_t codeQ8, uint32_t& mantQ8, int32_t& shift);
int64_t decodeQ8(uint16_t codeQ8);       // whole table units (floor)
double decodeQ8Double(uint16_t codeQ8);  // table units, fraction kept

// The FIG. 8 decoder. latch47 is the coefficient output latch together with its exponent
// latch 50; it is the operand ADDER 51 adds to a decoded difference.
struct Decoder {
    unsigned mantBits = 16;
    // true: ADDER 51 adds the two linear values exactly and renormalises. false (default):
    // shifters 52 and 53 align both operands to the larger exponent first.
    bool exactSum = false;
    BlockFloat latch47;

    void reset() { latch47 = BlockFloat(); }
    BlockFloat decode(uint16_t codeQ8, Role role);
};

// ---------------------------------------------------------------- mult.rs
enum class Rounding { Floor, Nearest };

struct MultParams {
    unsigned lsbDrop = 0;
    Rounding round = Rounding::Floor;
    unsigned productBits = 24;
    bool productSaturate = true;
};

// Stage 36 + BARREL SHIFTER 37: scale a raw operand*mantissa product by the exponent and
// realign to the data's binary point.
int64_t scaleProduct(int64_t raw, int32_t exp, const MultParams& p);

// FIG. 7 as a three-latch pipeline.
struct Multiplier {
    int64_t latch18 = 0;   // signed operand
    BlockFloat latch19;    // mantissa with the exponent from LATCH 50
    int64_t latch35 = 0;   // operand times mantissa
    int32_t latch34 = 0;   // exponent travelling with LATCH 35
    int64_t latch38 = 0;   // the product

    int64_t product() const { return latch38; }
    // Clock edge at the end of a cycle; LATCH 18/19 take the operand and coefficient if load.
    void clock(bool load, int64_t operand, BlockFloat coef, const MultParams& p);
};

// ---------------------------------------------------------------- section.rs
// Word widths and overflow behaviour.
struct Params {
    unsigned accBits = 24;        // ADDER 12, ACC 10, ACC 11, LATCH 38 and the busses
    bool accSaturate = false;     // clamp (true) or wrap (false) an adder result
    unsigned inputBits = 24;      // INPUT SIGNAL bus, sign-extended into ACC 10
    unsigned delayBits = 16;      // DELAY MEMORY 22 word; writes saturate
    unsigned multInBits = 16;     // LATCH 18 after dropping multLsbDrop low bits
    bool multInSaturate = true;   // clamp (true) or wrap (false) the operand
    unsigned multLsbDrop = 0;     // low bits removed from the operand before the multiply
    unsigned mantBits = 16;       // coefficient mantissa (LATCH 19)
    bool exactCoefSum = false;    // exact or aligned addition in the FIG. 8 decoder
    Rounding productRound = Rounding::Floor;
    bool productSaturate = true;

    MultParams mult() const {
        MultParams m;
        m.lsbDrop = multLsbDrop;
        m.round = productRound;
        m.productBits = accBits;
        m.productSaturate = productSaturate;
        return m;
    }
    int64_t accFix(int64_t v) const { return accSaturate ? sat(v, accBits) : wrap(v, accBits); }
    // Form the multiplier operand (LATCH 18) from a bus value.
    int64_t operand(int64_t v) const {
        unsigned full = multInBits + multLsbDrop;
        v = multInSaturate ? sat(v, full) : wrap(v, full);
        return v >> multLsbDrop;
    }
};

// Control word fields.
enum class Mux15 : uint8_t { Z2N, Z1N, Acc11, Prod };
enum class Mux16 : uint8_t { Z1x2, Acc10, Acc10N };
enum class Gate : uint8_t { Always, Dc, Idc, Never };
enum class Cin : uint8_t { Zero, One, Idc };
enum class Acc10Op : uint8_t { Hold, Adder, Input };
enum class Acc11Op : uint8_t { Hold, Adder, Mux16 };
enum class MuxM : uint8_t { Adder, Z2, Z1 };
// Coefficient memory slot; B1 and A1 hold difference codes, G the output gain. kNoCoef: no load.
enum Coef : int8_t { kCoefB2 = 0, kCoefB1 = 1, kCoefA2 = 2, kCoefA1 = 3, kCoefG = 4, kNoCoef = -1 };
Role coefRole(Coef c);

// One microcode word: everything the datapath needs for one cycle.
struct Ctl {
    const char* name;
    Mux15 mux15;
    Mux16 mux16;
    Gate gate;
    Cin cin;
    Acc10Op acc10;
    Acc11Op acc11;
    MuxM muxM;
    Coef load18;      // load LATCH 18 (from MuxM) and LATCH 19 (with this coefficient)
    bool dmemWrite;   // write ACC 11 into DELAY MEMORY 22 at the end of the cycle
    bool latchZ;      // load LATCH 13 and LATCH 20 from DELAY MEMORY 22 at the end of the cycle
};

// The reservation table: load state plus the eight rows; and with the three gain rows.
extern const Ctl kMicrocode[9];
extern const Ctl kMicrocodeGain[12];

// Counters of the places where a value was clipped or would have wrapped.
struct Stats {
    uint64_t delaySat = 0;
    uint64_t accOverflow = 0;
    uint64_t multInSat = 0;
    uint64_t productSat = 0;
};

// FIG. 6: one filter section, cycle by cycle.
class Section {
public:
    Params p;
    bool dc = false;           // DC gain stabilisation mode bit (FIG. 5 form when true)
    int64_t input = 0;         // INPUT SIGNAL bus
    int64_t acc10 = 0, acc11 = 0;
    int64_t latch13 = 0, latch20 = 0;  // Y[n-2], Y[n-1]
    int64_t dmem[2] = {0, 0};          // DELAY MEMORY 22: [Y[n-1], Y[n-2]]
    uint16_t coefMem[5];               // codes with 8 fraction bits: B2', B1'-B2', A2', A1'-A2', G
    bool withGain = false;             // run the gain rows after row 8
    Decoder decoder;
    Multiplier mult;
    Stats stats;

    explicit Section(const Params& params = Params());
    void setParams(const Params& params);
    // Clear the signal state (not the coefficients or parameters).
    void resetState();
    void setCodesQ8(uint16_t b2, uint16_t b1Diff, uint16_t a2, uint16_t a1Diff);
    void setGainQ8(uint16_t g);
    // The coefficients the multiplier will see, in microcode order: [B2', B1', A2', A1', G].
    void effectiveCoefs(BlockFloat out[5]) const;
    const Ctl* program(unsigned& n) const;
    void step(const Ctl& ctl);
    // Run the whole microcode for one input sample; returns ACC 10 after the last cycle.
    int64_t process(int64_t x);
};

// ---------------------------------------------------------------- voice.rs
constexpr uint32_t kNeutralWord = 0xFFCFFFCFu;
constexpr uint8_t kFlagGain = 0x40;         // word 9 is a gain, not a zero word
constexpr uint8_t kFlagUnnormalised = 0x80; // words 4 and 8 run unnormalised (1/D)

// C(x) = Ca + x (Cb - Ca) with x a 16-bit fraction of 65536, rounded to 8 fraction bits of a
// code. For a cutoff byte c, x = c * 0x0101 makes this c0 + (c255 - c0) * c / 255.
inline uint16_t interpX(uint8_t c0, uint8_t c255, uint16_t x) {
    int32_t a = c0, b = c255;
    return static_cast<uint16_t>((a * 65536 + (b - a) * int32_t(x) + 128) >> 8);
}
inline uint16_t dup(uint8_t v) { return static_cast<uint16_t>(v) * 0x0101u; }

// One E4 voice: ten 32-bit words, flags, and the interpolating variable x, on three sections.
// Section k uses pole word 4k and zero word 4k+1; with kFlagGain word 9 is the gain of section 2
// (its zeros neutral); with kFlagUnnormalised sections 1 and 2 run 1/D; section 0 is always f²/D.
class Voice {
public:
    explicit Voice(const Params& params = Params());
    void setParams(const Params& params);
    const Params& params() const { return p_; }
    // Program the words and flags; the codes are re-interpolated at the current x.
    void setWords(const uint32_t words[10], uint8_t flags);
    // The interpolating variable, 0..65535 (a cutoff byte c is dup(c)).
    void setX(uint16_t x);
    uint16_t x() const { return x_; }
    const uint32_t* words() const { return words_; }
    uint8_t flags() const { return flags_; }
    void resetState();
    int64_t process(int64_t in);
    Section& section(int k) { return sections_[k]; }
    const Section& section(int k) const { return sections_[k]; }
    // Sum of the sections' counters since the last resetStats().
    Stats stats() const;
    void resetStats();

private:
    void apply();
    Params p_;
    uint32_t words_[10];
    uint8_t flags_ = kFlagGain;
    uint16_t x_ = 0;
    Section sections_[3];
};

}  // namespace hchip
