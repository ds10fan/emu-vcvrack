#include "hchip.hpp"

#include <cmath>

namespace hchip {

// ---------------------------------------------------------------- coef.rs
int64_t BlockFloat::linear() const {
    return exp >= 0 ? (int64_t(mant) << exp) : (int64_t(mant) >> (-exp));
}

double BlockFloat::toDouble() const {
    return double(mant) * std::ldexp(1.0, exp) / double(uint64_t(1) << kFracBits);
}

BlockFloat normalised(uint64_t mant, int32_t exp, unsigned mantBits) {
    unsigned bits = 0;
    for (uint64_t m = mant; m; m >>= 1) bits++;
    unsigned drop = bits > mantBits ? bits - mantBits : 0;
    BlockFloat r;
    r.mant = static_cast<uint32_t>(mant >> drop);
    r.exp = exp + int32_t(drop);
    return r;
}

int64_t decodeLinear(uint8_t code) {
    int64_t c = int64_t(code) + 1;
    int64_t e = c >> 4;
    int64_t m = c & 15;
    if (c >= 224) return (c - 192) << 12;
    if (e != 0) return (16 + m) << (e - 1);
    return m;
}

void decodeQ8Parts(uint16_t codeQ8, uint32_t& mantQ8, int32_t& shift) {
    uint32_t c = (codeQ8 >> 8) + 1;
    uint32_t f = codeQ8 & 255;
    uint32_t e = c >> 4;
    uint32_t m = c & 15;
    if (c >= 224) {
        mantQ8 = ((c - 192) << 8) | f;
        shift = 12;
    } else if (e != 0) {
        mantQ8 = ((16 + m) << 8) | f;
        shift = int32_t(e) - 1;
    } else {
        mantQ8 = (m << 8) | f;
        shift = 0;
    }
}

int64_t decodeQ8(uint16_t codeQ8) {
    uint32_t m;
    int32_t s;
    decodeQ8Parts(codeQ8, m, s);
    return (int64_t(m) << s) >> 8;
}

double decodeQ8Double(uint16_t codeQ8) {
    uint32_t m;
    int32_t s;
    decodeQ8Parts(codeQ8, m, s);
    return double(m) * std::ldexp(1.0, s) / 256.0;
}

BlockFloat Decoder::decode(uint16_t codeQ8, Role role) {
    // LOGIC 43, BARREL SHIFTER 44, LATCH 45 / LATCH 46: the mantissa keeps the code's fraction
    // bits, the exponent accounts for them.
    uint32_t mantQ8;
    int32_t shift;
    decodeQ8Parts(codeQ8, mantQ8, shift);
    BlockFloat d = normalised(mantQ8, shift + int32_t(roleShift(role)) - 8, mantBits);
    BlockFloat out;
    if (role != Role::First) {
        out = d;
    } else if (exactSum) {
        int32_t e = d.exp < latch47.exp ? d.exp : latch47.exp;
        uint64_t sum = (uint64_t(d.mant) << (d.exp - e)) + (uint64_t(latch47.mant) << (latch47.exp - e));
        out = normalised(sum, e, mantBits);
    } else {
        BlockFloat s = latch47;
        int32_t e = d.exp > s.exp ? d.exp : s.exp;
        uint32_t a = d.mant >> (e - d.exp);  // BARREL SHIFTER 52
        uint32_t b = s.mant >> (e - s.exp);  // BARREL SHIFTER 53
        out = normalised(uint64_t(a) + b, e, mantBits);  // ADDER 51
    }
    latch47 = out;
    return out;
}

// ---------------------------------------------------------------- mult.rs
int64_t scaleProduct(int64_t raw, int32_t exp, const MultParams& p) {
    int64_t shift = int64_t(kFracBits) - exp - int64_t(p.lsbDrop);
    int64_t v;
    if (shift > 0) {
        v = p.round == Rounding::Floor ? (raw >> shift) : ((raw + (int64_t(1) << (shift - 1))) >> shift);
    } else {
        v = static_cast<int64_t>(static_cast<uint64_t>(raw) << (-shift));
    }
    return p.productSaturate ? sat(v, p.productBits) : wrap(v, p.productBits);
}

void Multiplier::clock(bool load, int64_t operand, BlockFloat coef, const MultParams& p) {
    latch38 = scaleProduct(latch35, latch34, p);
    latch35 = latch18 * int64_t(latch19.mant);
    latch34 = latch19.exp;
    if (load) {
        latch18 = operand;
        latch19 = coef;
    }
}

// ---------------------------------------------------------------- microcode.rs
Role coefRole(Coef c) {
    switch (c) {
        case kCoefB2: case kCoefA2: return Role::Second;
        case kCoefB1: case kCoefA1: return Role::First;
        default: return Role::Gain;
    }
}

namespace {
constexpr Ctl ROW0 = {"0 load", Mux15::Z2N, Mux16::Acc10, Gate::Dc, Cin::One, Acc10Op::Input, Acc11Op::Hold, MuxM::Adder, kNoCoef, false, true};
// Z2' + (SA = Xn) & DC + 1 -> LATCH 18, coefficient B2'
constexpr Ctl ROW1 = {"1", Mux15::Z2N, Mux16::Acc10, Gate::Dc, Cin::One, Acc10Op::Hold, Acc11Op::Hold, MuxM::Adder, kCoefB2, false, false};
// Z1' + (SA = Xn) & DC + 1 -> LATCH 18, coefficient B1'
constexpr Ctl ROW2 = {"2", Mux15::Z1N, Mux16::Acc10, Gate::Dc, Cin::One, Acc10Op::Hold, Acc11Op::Hold, MuxM::Adder, kCoefB1, false, false};
// Z2' + 2Z1 + 1 = 2Z1 - Z2 -> ACC 11
constexpr Ctl ROW3 = {"3", Mux15::Z2N, Mux16::Z1x2, Gate::Always, Cin::One, Acc10Op::Hold, Acc11Op::Adder, MuxM::Adder, kNoCoef, false, false};
// b2'(..) + (SA = Xn') & IDC + IDC = -AR2 -> ACC 10; Z2 -> LATCH 18, coefficient A2'
constexpr Ctl ROW4 = {"4", Mux15::Prod, Mux16::Acc10N, Gate::Idc, Cin::Idc, Acc10Op::Adder, Acc11Op::Hold, MuxM::Z2, kCoefA2, false, false};
// b1'(..) + (-AR2)' + 1 = AR -> ACC 10; Z1 -> LATCH 18, coefficient A1'
constexpr Ctl ROW5 = {"5", Mux15::Prod, Mux16::Acc10N, Gate::Always, Cin::One, Acc10Op::Adder, Acc11Op::Hold, MuxM::Z1, kCoefA1, false, false};
// (2Z1 - Z2) + AR + 0 = Yn -> ACC 11
constexpr Ctl ROW6 = {"6", Mux15::Acc11, Mux16::Acc10, Gate::Always, Cin::Zero, Acc10Op::Hold, Acc11Op::Adder, MuxM::Adder, kNoCoef, false, false};
// a2'Z2 + AR' + 1 = -MA1 -> ACC 10; Yn -> DELAY MEMORY
constexpr Ctl ROW7 = {"7", Mux15::Prod, Mux16::Acc10N, Gate::Always, Cin::One, Acc10Op::Adder, Acc11Op::Hold, MuxM::Adder, kNoCoef, true, false};
// a1'Z1 + (-MA1)' + 1 = MAout -> ACC 10
constexpr Ctl ROW8 = {"8", Mux15::Prod, Mux16::Acc10N, Gate::Always, Cin::One, Acc10Op::Adder, Acc11Op::Hold, MuxM::Adder, kNoCoef, false, false};
// Gain rows: row 8 additionally latches MAout with the gain coefficient.
constexpr Ctl ROW8G = {"8g", Mux15::Prod, Mux16::Acc10N, Gate::Always, Cin::One, Acc10Op::Adder, Acc11Op::Hold, MuxM::Adder, kCoefG, false, false};
// Wait for the multiplier pipeline; everything holds.
constexpr Ctl ROW9 = {"9", Mux15::Prod, Mux16::Acc10, Gate::Never, Cin::Zero, Acc10Op::Hold, Acc11Op::Hold, MuxM::Adder, kNoCoef, false, false};
constexpr Ctl ROW10 = {"10", Mux15::Prod, Mux16::Acc10, Gate::Never, Cin::Zero, Acc10Op::Hold, Acc11Op::Hold, MuxM::Adder, kNoCoef, false, false};
// g * MAout + 0 + 0 -> ACC 10
constexpr Ctl ROW11 = {"11", Mux15::Prod, Mux16::Acc10, Gate::Never, Cin::Zero, Acc10Op::Adder, Acc11Op::Hold, MuxM::Adder, kNoCoef, false, false};
}  // namespace

const Ctl kMicrocode[9] = {ROW0, ROW1, ROW2, ROW3, ROW4, ROW5, ROW6, ROW7, ROW8};
const Ctl kMicrocodeGain[12] = {ROW0, ROW1, ROW2, ROW3, ROW4, ROW5, ROW6, ROW7, ROW8G, ROW9, ROW10, ROW11};

// ---------------------------------------------------------------- section.rs
Section::Section(const Params& params) {
    coefMem[0] = uint16_t(kCodeOneX) << 8;
    coefMem[1] = uint16_t(kCodeOneF) << 8;
    coefMem[2] = uint16_t(kCodeOneX) << 8;
    coefMem[3] = uint16_t(kCodeOneF) << 8;
    coefMem[4] = uint16_t(kCodeOneX) << 8;
    setParams(params);
}

void Section::setParams(const Params& params) {
    p = params;
    decoder.mantBits = p.mantBits;
    decoder.exactSum = p.exactCoefSum;
    decoder.reset();
}

void Section::resetState() {
    input = 0;
    acc10 = acc11 = 0;
    latch13 = latch20 = 0;
    dmem[0] = dmem[1] = 0;
    decoder.reset();
    mult = Multiplier();
    stats = Stats();
}

void Section::setCodesQ8(uint16_t b2, uint16_t b1Diff, uint16_t a2, uint16_t a1Diff) {
    coefMem[0] = b2;
    coefMem[1] = b1Diff;
    coefMem[2] = a2;
    coefMem[3] = a1Diff;
}

void Section::setGainQ8(uint16_t g) {
    coefMem[4] = g;
    withGain = true;
}

void Section::effectiveCoefs(BlockFloat out[5]) const {
    Decoder d;
    d.mantBits = p.mantBits;
    d.exactSum = p.exactCoefSum;
    const Coef order[5] = {kCoefB2, kCoefB1, kCoefA2, kCoefA1, kCoefG};
    for (int i = 0; i < 5; i++) out[i] = d.decode(coefMem[order[i]], coefRole(order[i]));
}

const Ctl* Section::program(unsigned& n) const {
    if (withGain) {
        n = 12;
        return kMicrocodeGain;
    }
    n = 9;
    return kMicrocode;
}

void Section::step(const Ctl& ctl) {
    const int64_t z2 = latch13;
    const int64_t z1 = latch20;

    // MUX 15 -> adder input 14
    int64_t a14;
    switch (ctl.mux15) {
        case Mux15::Z2N: a14 = notb(z2, p.accBits); break;
        case Mux15::Z1N: a14 = notb(z1, p.accBits); break;
        case Mux15::Acc11: a14 = acc11; break;
        default: a14 = mult.product(); break;
    }
    // MUX 16 and its gate -> adder input 17
    int64_t m16;
    switch (ctl.mux16) {
        case Mux16::Z1x2: m16 = wrap(z1 * 2, p.accBits); break;
        case Mux16::Acc10: m16 = acc10; break;
        default: m16 = notb(acc10, p.accBits); break;
    }
    bool pass;
    switch (ctl.gate) {
        case Gate::Always: pass = true; break;
        case Gate::Dc: pass = dc; break;
        case Gate::Idc: pass = !dc; break;
        default: pass = false; break;
    }
    const int64_t a17 = pass ? m16 : 0;
    int64_t cin;
    switch (ctl.cin) {
        case Cin::Zero: cin = 0; break;
        case Cin::One: cin = 1; break;
        default: cin = dc ? 0 : 1; break;
    }
    // ADDER 12
    const int64_t raw = a14 + a17 + cin;
    if (overflows(raw, p.accBits)) stats.accOverflow++;
    const int64_t sum = p.accFix(raw);
    // MUXM -> LATCH 18 data
    int64_t mm;
    switch (ctl.muxM) {
        case MuxM::Adder: mm = sum; break;
        case MuxM::Z2: mm = z2; break;
        default: mm = z1; break;
    }

    // Clock edge.
    bool load = ctl.load18 != kNoCoef;
    int64_t operand = 0;
    BlockFloat coef;
    if (load) {
        operand = p.operand(mm);
        if (operand != (mm >> p.multLsbDrop)) stats.multInSat++;
        coef = decoder.decode(coefMem[ctl.load18], coefRole(Coef(ctl.load18)));
    }
    {
        MultParams full = p.mult();
        full.productBits = 62;
        full.productSaturate = false;
        if (overflows(scaleProduct(mult.latch35, mult.latch34, full), p.accBits)) stats.productSat++;
    }
    mult.clock(load, operand, coef, p.mult());
    switch (ctl.acc10) {
        case Acc10Op::Hold: break;
        case Acc10Op::Adder: acc10 = sum; break;
        default: acc10 = wrap(input, p.inputBits < p.accBits ? p.inputBits : p.accBits); break;
    }
    switch (ctl.acc11) {
        case Acc11Op::Hold: break;
        case Acc11Op::Adder: acc11 = sum; break;
        default: acc11 = a17; break;
    }
    if (ctl.dmemWrite) {
        int64_t v = sat(acc11, p.delayBits);
        if (v != acc11) stats.delaySat++;
        dmem[1] = dmem[0];
        dmem[0] = v;
    }
    if (ctl.latchZ) {
        latch20 = dmem[0];
        latch13 = dmem[1];
    }
}

int64_t Section::process(int64_t x) {
    input = x;
    unsigned n;
    const Ctl* prog = program(n);
    for (unsigned i = 0; i < n; i++) step(prog[i]);
    return acc10;
}

// ---------------------------------------------------------------- voice.rs
namespace {
const int POLE_WORDS[3] = {0, 4, 8};
const int ZERO_WORDS[3] = {1, 5, 9};
inline void wordCodes(uint32_t w, uint8_t& x0, uint8_t& f0, uint8_t& x255, uint8_t& f255) {
    x0 = uint8_t(w >> 24);
    f0 = uint8_t(w >> 16);
    x255 = uint8_t(w >> 8);
    f255 = uint8_t(w);
}
}  // namespace

Voice::Voice(const Params& params) : p_(params), sections_{Section(params), Section(params), Section(params)} {
    for (int i = 0; i < 10; i++) words_[i] = kNeutralWord;
    words_[9] = 0x00FF00FFu;
    apply();
}

void Voice::setParams(const Params& params) {
    p_ = params;
    for (auto& s : sections_) s.setParams(params);
}

void Voice::setWords(const uint32_t words[10], uint8_t flags) {
    for (int i = 0; i < 10; i++) words_[i] = words[i];
    flags_ = flags;
    apply();
}

void Voice::setX(uint16_t x) {
    if (x == x_) return;
    x_ = x;
    apply();
}

// Interpolate the codes at x and load the three sections (the coefficient math unit, four
// codes per section).
void Voice::apply() {
    const bool gainMode = (flags_ & kFlagGain) != 0;
    const bool unnorm = (flags_ & kFlagUnnormalised) != 0;
    for (int k = 0; k < 3; k++) {
        uint8_t px0, pf0, px255, pf255;
        wordCodes(words_[POLE_WORDS[k]], px0, pf0, px255, pf255);
        uint16_t b2 = interpX(px0, px255, x_);
        uint16_t b1 = interpX(pf0, pf255, x_);
        const bool thirdIsGain = k == 2 && gainMode;
        uint16_t a2, a1;
        if (thirdIsGain) {
            a2 = uint16_t(kCodeOneX) << 8;
            a1 = uint16_t(kCodeOneF) << 8;
        } else {
            uint8_t zx0, zf0, zx255, zf255;
            wordCodes(words_[ZERO_WORDS[k]], zx0, zf0, zx255, zf255);
            a2 = interpX(zx0, zx255, x_);
            a1 = interpX(zf0, zf255, x_);
        }
        Section& s = sections_[k];
        s.dc = !(k > 0 && unnorm);
        s.setCodesQ8(b2, b1, a2, a1);
        if (thirdIsGain) {
            uint8_t gx0, g0, gx255, g255;
            wordCodes(words_[9], gx0, g0, gx255, g255);
            s.setGainQ8(interpX(g0, g255, x_));
        } else {
            s.withGain = false;
        }
    }
}

void Voice::resetState() {
    for (auto& s : sections_) s.resetState();
}

int64_t Voice::process(int64_t in) {
    int64_t v = in;
    for (auto& s : sections_) v = s.process(v);
    return v;
}

Stats Voice::stats() const {
    Stats t;
    for (const auto& s : sections_) {
        t.delaySat += s.stats.delaySat;
        t.accOverflow += s.stats.accOverflow;
        t.multInSat += s.stats.multInSat;
        t.productSat += s.stats.productSat;
    }
    return t;
}

void Voice::resetStats() {
    for (auto& s : sections_) s.stats = Stats();
}

}  // namespace hchip
