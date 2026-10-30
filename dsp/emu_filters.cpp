// The filter recipes, ported from the decompiled EOS 3.20e code (ghidra/out/filters.c, addresses in the
// comments) and from the OBXa patch's reference model (crates/eos320/src/reference.rs). Every table value
// comes from the 3.20e image (E-MU_EOS_3.20e.img, flash address - 0x10000 = file offset) or from
// tables/tables.json. Arithmetic follows the 68020 code: 32-bit two's complement, arithmetic shifts.
#include "emu_filters.hpp"

#include <cstdio>
#include <cstdlib>

namespace emuz {
namespace {

// ---------------------------------------------------------------- stock tables (flash addresses)
// DAT_000804a0: RES16, the lowpass pole section per resonance step (q >> 3).
const uint32_t RES16[16] = {
    0x8408FFCF, 0x8008FBD1, 0x7D08F8D4, 0x7A08F5D6, 0x7708F2D9, 0x7308EFDB, 0x7008ECDE, 0x6D07E9E0,
    0x6A07E6E3, 0x6707E3E6, 0x6307E0E8, 0x6006DDEB, 0x5D06DAED, 0x5A05D7F0, 0x5205D4F3, 0x5205D4F3,
};
// filt_tab_gainOffset @ 0x80460: GOFF, subtracted from the gain word (2 units of the low frame per step).
const uint32_t GOFF[16] = {
    0x000000, 0x020000, 0x040000, 0x060000, 0x080000, 0x0A0000, 0x0C0000, 0x0E0000,
    0x100000, 0x120000, 0x140000, 0x160000, 0x180000, 0x1A0000, 0x1C0000, 0x1E0000,
};
// DAT_000804e0: RGAIN, the lowpass gain-compensation return value.
const int32_t RGAIN[16] = {0, -1, -2, -3, -5, -6, -7, -9, -10, -12, -13, -15, -17, -19, -21, -21};
// filt_tab_resSection16 @ 0x80F28: the second RES16 (byte-identical to 4.70's 0x6875C), used by the
// 2EQ+Lowpass Morph only.
const uint32_t RES16B[16] = {
    0x8408FFCF, 0x8008FBD2, 0x7D08F8D5, 0x7A08F5D8, 0x7708F2DB, 0x7308F0DD, 0x7008EEE0, 0x6D07ECE2,
    0x6A07E8E4, 0x6707E4E7, 0x6307E2E8, 0x6006E0E9, 0x5D06DFEB, 0x5A05DEEC, 0x5205DCED, 0x5205DCED,
};
// DAT_00f01d94: the vocal filters' return value is -(this byte - 3).
const int VOCAL_GAIN_BYTE = 0x0A;

// ---------------------------------------------------------------- OBXa tables (tables/tables.json)
const uint32_t LP_SVF[32] = {
    0x8600FBD3, 0x8500FBD3, 0x8400FAD4, 0x8300FAD4, 0x8300FAD4, 0x8200FAD4, 0x8100F9D4, 0x8000F9D4,
    0x7F00F8D5, 0x7E00F8D5, 0x7D00F8D5, 0x7C00F7D6, 0x7B00F6D6, 0x7A00F6D6, 0x7900F5D7, 0x7800F5D7,
    0x7700F4D8, 0x7500F3D8, 0x7400F2D9, 0x7200F1DA, 0x7100F0DA, 0x6F00EFDB, 0x6D00EEDC, 0x6B00ECDD,
    0x6900EADE, 0x6600E8E0, 0x6300E6E1, 0x6000E3E3, 0x5C00E0E5, 0x5800DCE8, 0x5200D7EB, 0x4B00D1EF,
};
const uint32_t LP_LADA[32] = {
    0x8408FFCF, 0x8408FFCF, 0x8008FBD2, 0x8008FBD2, 0x7D08F8D4, 0x7D07F8D4, 0x7A07F5D6, 0x7907F4D7,
    0x7606F2D9, 0x7506F1DA, 0x7105EEDB, 0x7005EEDC, 0x6D05EBDD, 0x6B04EADE, 0x6803E8E0, 0x6703E7E0,
    0x6403E4E2, 0x6202E3E3, 0x5F02E1E4, 0x5D02DFE6, 0x5A01DDE7, 0x5701DBE8, 0x5400D8EA, 0x5000D5EC,
    0x4D00D2ED, 0x4900CFEE, 0x4500CBEF, 0x4100C7F0, 0x3B00C1F1, 0x3300BAF2, 0x2700AEF3, 0x0D0094F4,
};
const uint32_t LP_LADB16[16] = {
    0x8408FFCF, 0x8008FBD2, 0x7F07F9D4, 0x8007F7D5, 0x8106F6D6, 0x8205F5D7, 0x8404F5D7, 0x8703F6D6,
    0x8A02F7D6, 0x8F02F9D4, 0x9301FBD3, 0x9A00FED1, 0x9C00FFD0, 0x9C00FFD0, 0x9D00FFD0, 0x9E00FFD0,
};
const uint32_t LP6_CAP = 30;
const uint32_t HP_SVF[32] = {
    0x8A00FAD4, 0x8900FAD4, 0x8900F9D4, 0x8800F9D4, 0x8700F9D4, 0x8600F8D5, 0x8500F8D5, 0x8500F8D5,
    0x8400F7D6, 0x8300F7D6, 0x8200F6D6, 0x8100F6D6, 0x8000F5D7, 0x7F00F5D7, 0x7E00F4D8, 0x7C00F3D8,
    0x7B00F2D9, 0x7A00F1DA, 0x7800F1DA, 0x7700EFDB, 0x7500EEDC, 0x7300EDDC, 0x7200ECDD, 0x6F00EADE,
    0x6D00E8E0, 0x6B00E6E1, 0x6800E4E2, 0x6500E1E4, 0x6100DEE6, 0x5C00DAE9, 0x5700D5EC, 0x4F00CEEF,
};
const uint32_t HP_LADA[32] = {
    0x8410FFCA, 0x8410FFCA, 0x8010FBCD, 0x8010FBCD, 0x7D10F8CF, 0x7D0FF8D0, 0x7A0EF5D2, 0x7A0DF4D3,
    0x770CF1D6, 0x760CF1D6, 0x720BEED9, 0x720AEDD9, 0x6F09EADB, 0x6E08E9DC, 0x6B07E7DF, 0x6A06E6E0,
    0x6706E3E1, 0x6505E2E2, 0x6304DFE3, 0x6003DEE4, 0x5D03DBE5, 0x5B02D9E6, 0x5801D7E7, 0x5400D3E8,
    0x5100D0E9, 0x4E00CDEA, 0x4A00C9EA, 0x4500C5EB, 0x3F00BFEC, 0x3700B7ED, 0x2B00ACEE, 0x120092EF,
};
const uint32_t HP_LADB[8] = {
    0x8010FBCD, 0x7F0EF6D1, 0x810BF4D4, 0x8707F5D5, 0x8F04F7D4, 0x9A01FCD2, 0xA000FFD0, 0xA200FFD0,
};
const uint32_t BP_SVF[32] = {
    0x8600F3C6, 0x8500F3C8, 0x8400F2C9, 0x8300F2CA, 0x8300F1CB, 0x8200F1CC, 0x8100F0CC, 0x8000F0CD,
    0x7F00EFCE, 0x7E00EECE, 0x7D00EECF, 0x7C00EDD0, 0x7B00ECD0, 0x7A00EBD1, 0x7900EBD1, 0x7800EAD2,
    0x7700E9D2, 0x7500E8D2, 0x7400E6D3, 0x7200E5D3, 0x7100E4D3, 0x6F00E2D4, 0x6D00E1D4, 0x6B00DFD4,
    0x6900DDD4, 0x6600DBD4, 0x6300D8D5, 0x6000D5D5, 0x5C00D2D5, 0x5800CED5, 0x5200C9D5, 0x4B00C2D5,
};
const uint32_t BP_LADA[32] = {
    0x8708F7C8, 0x8508F5C8, 0x8308F3C8, 0x8108F1C8, 0x7F08EFC8, 0x7D07EDC8, 0x7A07EBC8, 0x7807E8C8,
    0x7606E6C8, 0x7306E4C8, 0x7105E2C8, 0x6E05E0C8, 0x6C05DEC8, 0x6A04DCC8, 0x6704DAC8, 0x6503D8C9,
    0x6303D6C9, 0x6003D4C9, 0x5E02D2CA, 0x5C02D0CA, 0x5901CECB, 0x5601CBCC, 0x5401C9CD, 0x5000C6CE,
    0x4D00C3CF, 0x4900C0D0, 0x4500BCD0, 0x4100B7D1, 0x3B00B2D2, 0x3300AAD3, 0x27009ED4, 0x0D0084D5,
};
const uint32_t BP_LADB[16] = {
    0x8708F7C8, 0x8308F3C8, 0x8008EFC8, 0x7F07EDC8, 0x7F06ECC8, 0x8005EBC8, 0x8105EBC8, 0x8404ECC8,
    0x8703EEC9, 0x8B02F1CA, 0x9001F4CB, 0x9601F9CD, 0x9B00FDCF, 0x9C00FDD0, 0x9D00FDD2, 0x9D00FDD2,
};
const uint32_t CBP[32] = {
    0x8800D0C6, 0x8400CCC8, 0x8000C8C9, 0x7C00C4CA, 0x7800C0CB, 0x7400BCCC, 0x7000B8CC, 0x6C00B4CD,
    0x6800B0CE, 0x6400ACCE, 0x6000A8CF, 0x5C00A4D0, 0x5800A0D0, 0x54009CD1, 0x500098D1, 0x4C0094D2,
    0x480090D2, 0x44008CD2, 0x400088D3, 0x3C0084D3, 0x380080D3, 0x34007CD4, 0x300078D4, 0x2C0074D4,
    0x280070D4, 0x24006CD4, 0x200068D5, 0x1C0064D5, 0x180060D5, 0x14005CD5, 0x100058D5, 0x090051D5,
};
const uint32_t BP4_BOOST = 11;

const uint32_t HP_ZERO = 0x80048004;
const uint32_t BP_W01 = 0xFF68FFCF;
const uint32_t BP_ZERO = 0xFF04FF90;
const uint32_t CBP_ZERO = 0xFFD0FF20;

// ---------------------------------------------------------------- descriptors (menu order)
const FilterDesc DESCS[kNumFilters] = {
    {"2 Pole Low-pass", 0x01, 2, FreqLaw::Lowpass, "Frequency", "Q", false},
    {"4 Pole Low-pass", 0x00, 2, FreqLaw::Lowpass, "Frequency", "Q", false},
    {"6 Pole Low-pass", 0x02, 2, FreqLaw::Lowpass, "Frequency", "Q", false},
    {"2nd Order High-pass", 0x08, 2, FreqLaw::Highpass, "Frequency", "Q", false},
    {"4th Order High-pass", 0x09, 2, FreqLaw::Highpass, "Frequency", "Q", false},
    {"2nd Order Band-pass", 0x10, 2, FreqLaw::Bandpass, "Frequency", "Q", false},
    {"4th Order Band-pass", 0x11, 2, FreqLaw::Bandpass, "Frequency", "Q", false},
    {"Contrary Band-pass", 0x12, 2, FreqLaw::Bandpass, "Frequency", "Q", false},
    {"Swept EQ 1 octave", 0x20, 2, FreqLaw::Shelf, "Frequency", "Gain", true},
    {"Swept EQ 2->1 oct", 0x21, 2, FreqLaw::Shelf, "Frequency", "Gain", true},
    {"Swept EQ 3->1 oct", 0x22, 2, FreqLaw::Shelf, "Frequency", "Gain", true},
    {"Phaser 1", 0x40, 2, FreqLaw::Raw, "Frequency", "Resonance", false},
    {"Phaser 2", 0x41, 2, FreqLaw::Raw, "Frequency", "Resonance", false},
    {"Bat-Phaser", 0x42, 2, FreqLaw::Raw, "Frequency", "Resonance", false},
    {"Flanger Lite", 0x48, 2, FreqLaw::Raw, "Frequency", "Resonance", false},
    {"Vocal Ah-Ay-Ee", 0x50, 2, FreqLaw::Raw, "Morph", "Body Size", false},
    {"Vocal Oo-Ah", 0x51, 2, FreqLaw::Raw, "Morph", "Body Size", false},
    {"Dual EQ Morph", 0x60, 8, FreqLaw::Raw, "Morph", "Gain", true},
    {"2EQ+Lowpass Morph", 0x61, 8, FreqLaw::Raw, "Fc / Morph", "LPF Q", false},
    {"2EQMorph+Exprssn", 0x62, 8, FreqLaw::Raw, "Morph", "Expression", false},
    {"Peak/Shelf Morph", 0x68, 8, FreqLaw::Raw, "Morph", "Peak", true},
    {"OBXa 2 Pole LP", 0x04, 2, FreqLaw::Lowpass, "Frequency", "Q", false},
    {"OBXa 4 Pole LP", 0x05, 2, FreqLaw::Lowpass, "Frequency", "Q", false},
    {"OBXa 6 Pole LP", 0x06, 2, FreqLaw::Lowpass, "Frequency", "Q", false},
    {"OBXa 2nd Ord HP", 0x0A, 2, FreqLaw::Highpass, "Frequency", "Q", false},
    {"OBXa 4th Ord HP", 0x0B, 2, FreqLaw::Highpass, "Frequency", "Q", false},
    {"OBXa 2nd Ord BP", 0x13, 2, FreqLaw::Bandpass, "Frequency", "Q", false},
    {"OBXa 4th Ord BP", 0x14, 2, FreqLaw::Bandpass, "Frequency", "Q", false},
    {"OBXa Contrary BP", 0x15, 2, FreqLaw::Bandpass, "Frequency", "Q", false},
    {"4 Pole LP+Fuzz", 0x03, 2, FreqLaw::Lowpass, "Frequency", "Fuzz", false},
    {"2 Pole LP+Fuzz", 0x07, 2, FreqLaw::Lowpass, "Frequency", "Fuzz", false},
    {"2nd Ord HP+Fuzz", 0x0C, 2, FreqLaw::Highpass, "Frequency", "Fuzz", false},
    {"2nd Ord BP+Fuzz", 0x16, 2, FreqLaw::Bandpass, "Frequency", "Fuzz", false},
    {"Contrary BP+Fuzz", 0x17, 2, FreqLaw::Bandpass, "Frequency", "Fuzz", false},
};

// Arithmetic shift right (the 68020's asr), independent of the host compiler.
inline int32_t asr(int32_t v, int n) { return v < 0 ? ~((~v) >> n) : (v >> n); }
inline uint32_t u(int32_t v) { return static_cast<uint32_t>(v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

Block blank(uint32_t fill) {
    Block b;
    for (int i = 0; i < 10; i++) b.w[i] = fill;
    b.ret = 0;
    b.flags = 0;
    return b;
}

// ---------------------------------------------------------------- stock filters
// Filter_2_Pole_Low_pass_computeFrames @ 0x80520, variant 1/2/3 = 2/4/6 poles.
void lp(Block& b, int var, int q) {
    int q2 = q;
    if (var == 3) q2 = q - asr(q, 2);
    unsigned idx = (q2 & 0x7F) >> 3;
    uint32_t s = RES16[idx];
    b.flags = 0x40;
    b.w[1] = b.w[5] = kNeutral;
    if (var < 2) {
        b.w[9] = kGainBase - (GOFF[idx] >> 1);
        b.w[0] = s;
        b.w[4] = b.w[8] = kNeutral;
        b.ret = asr(RGAIN[idx], 1);
    } else if (var == 2) {
        b.w[9] = kGainBase - GOFF[idx];
        b.w[0] = b.w[4] = s;
        b.w[8] = kNeutral;
        b.ret = RGAIN[idx];
    } else {
        b.w[9] = kGainBase - GOFF[idx] - (GOFF[idx] >> 1);
        b.w[0] = b.w[4] = b.w[8] = s;
        b.ret = RGAIN[idx] * 2;
    }
}

// Filter_2nd_Order_High_pass_computeFrames @ 0x806dc, variant 1/2.
void hp(Block& b, int var, int q) {
    b.w[0] = kNeutral;
    b.w[1] = HP_ZERO;
    uint32_t p = 0x8004FFCFu - u(asr(q, 2)) * 0x01000100u;
    b.w[4] = p;
    if (var < 2) {
        b.w[5] = b.w[8] = kNeutral;
    } else {
        b.w[5] = HP_ZERO;
        b.w[8] = p;
    }
    b.w[9] = kGainBase;
    b.flags = 0xC0;
    b.ret = 0;
}

// Filter_Swept_EQ_1_octave_computeFrames @ 0x807ee, variant 1/2/3.
void sweptEq(Block& b, int var, int q) {
    int32_t i = asr(q - 0x40, 1);
    b.w[0] = b.w[1] = b.w[4] = kNeutral;
    b.w[9] = kGainBase;
    int32_t c = var < 2 ? 0x77 : (var == 2 ? 0x91 : 0xA4);
    b.w[5] = (u(i + 0xDF) * 0x100u) | 0x700D7u | (u(i + c) * 0x1000000u);
    b.w[8] = (u(0xDF - i) * 0x100u) | 0x700D7u | (u(c - i) * 0x1000000u);
    b.flags = 0xC0;
    b.ret = -3;
}

// Filter_2nd_Order_Band_pass_computeFrames @ 0x809b2, variant 1/2/3 (2nd, 4th, Contrary).
void bp(Block& b, int var, int q) {
    int32_t h = asr(q, 1);
    uint32_t sec = (u(0xF8 - h) * 0x100u) | 0x800C8u | (u(0x88 - h) * 0x1000000u);
    b.w[9] = kGainBase;
    b.flags = 0xC0;
    if (var < 2) {
        b.w[0] = b.w[1] = BP_W01;
        b.w[4] = kNeutral;
        b.w[5] = BP_ZERO;
        b.w[8] = sec;
        b.ret = -asr(q, 3);
    } else if (var == 2) {
        b.w[0] = sec;
        b.w[1] = BP_ZERO;
        b.w[4] = kNeutral;
        b.w[5] = BP_ZERO;
        b.w[8] = sec;
        b.ret = -asr(q, 2);
    } else {
        b.w[0] = (u(0xD0 - q) * 0x100u) | 0x800C8u | (u(0x88 - q) * 0x1000000u);
        b.w[1] = kNeutral;
        b.w[4] = kNeutral;
        b.w[5] = CBP_ZERO;
        b.w[8] = kNeutral;
        b.ret = 0;
    }
}

// Filter_Phaser_1_computeFrames @ 0x80b66, variant 1/2.
void phaser(Block& b, int var, int q) {
    uint32_t v = u(asr(q, 3)) | (u(asr(q, 1)) << 8);
    v = (v << 16) | v;
    b.w[0] = kNeutral;
    uint32_t t;
    if (var < 2) {
        b.w[1] = 0x309030D0u;
        b.w[4] = 0xC090C0D0u - v;  // -0x3f6f3f30
        b.w[5] = 0x30503090u;
        t = 0xC050C090u;           // -0x3faf3f70
    } else {
        b.w[1] = 0x309830F2u;
        b.w[4] = 0xC098C0F2u - v;  // -0x3f673f0e
        b.w[5] = 0x303030B0u;
        t = 0xC030C0B0u;           // -0x3fcf3f50
    }
    b.w[8] = t - v;
    b.w[9] = kGainBase;
    b.flags = 0xC0;
    b.ret = -asr(q, 3);
}

// Filter_Bat_Phaser_computeFrames @ 0x80c0e.
void batPhaser(Block& b, int q) {
    b.w[0] = kNeutral;
    b.w[1] = 0x001000C0u;
    uint32_t u1 = u(0x70 - asr(q, 1)) * 0x1000000u;
    uint32_t u2 = u(asr(q, 1) - 0xB0) * 0xFFFFFF00u;  // * -0x100
    b.w[4] = u1 | u2 | 0x001400C4u;
    b.w[5] = 0x003000E0u;
    b.w[8] = u2 | 0x002C00DCu | u1;
    b.w[9] = kGainBase;
    b.flags = 0xC0;
    b.ret = 0;
}

// Filter_Flanger_Lite_computeFrames @ 0x80c88 (variant 1 < 0x3e9 always).
void flanger(Block& b, int q) {
    uint32_t u1 = (u(asr(q, 4)) << 16) | u(asr(q, 4));
    uint32_t u2 = (u(asr(q, 2)) << 24) | (u(asr(q, 2)) << 8);
    b.w[0] = 0xFFCFFAC0u;
    b.w[1] = 0x240F546Fu;
    b.w[4] = (u1 + 0x8F13C074u) - u2;
    b.w[5] = 0x1403444Fu;
    b.w[8] = (u1 + 0x7803A852u) - u2;
    b.w[9] = kGainBase;
    b.flags = 0xC0;
    b.ret = 0;
}

// Filter_Vocal_Ah_Ay_Ee_computeFrames @ 0x80dd4 / Filter_Vocal_Oo_Ah_computeFrames @ 0x80e48.
void vocal(Block& b, int which, int q) {
    uint32_t v = u(asr(q, 2)) | (u(asr(q, 2)) << 16);
    if (which == 0) {
        b.w[0] = v + 0x709570B1u;
        b.w[4] = v + 0x706F709Du;
        b.w[8] = v + 0x70547035u;
    } else {
        b.w[0] = v + 0x90A39099u;
        b.w[4] = v + 0x8060908Au;
        b.w[8] = v + 0x50347060u;
    }
    b.w[1] = 0xFFC0FFC0u;
    b.w[5] = kNeutral;
    b.w[9] = kNeutral;
    b.flags = 0x00;
    b.ret = -(VOCAL_GAIN_BYTE - 3);
}

// Filter_Dual_EQ_Morph_computeFrames @ 0x80f68, variant 1 Dual EQ, 2 2EQ+Lowpass, 3 2EQMorph+Exprssn.
// s[0..5] are the struct bytes +6..+11: EQ1 low F, EQ1 high F, EQ1 gain, EQ2 low F, EQ2 high F, EQ2 gain.
void dualEq(Block& b, int var, int q, const uint8_t s[6]) {
    int16_t off;
    if (var == 2) {
        b.w[0] = RES16B[(asr(q, 3)) & 15];
        off = 0;
    } else if (var == 3) {
        uint32_t v = u((asr(q * 199, 7)) + 8) | (u((asr(q * 0x7B, 7)) + 0x84) * 0x100u);
        b.w[0] = (v << 16) | v;
        off = 0;
    } else {
        b.w[0] = kNeutral;
        off = static_cast<int16_t>(asr(q - 0x40, 1));
    }
    b.w[9] = kGainBase;
    auto sc = [](uint8_t c) { return static_cast<int>(static_cast<int8_t>(c)); };
    int g1 = clampi(static_cast<int16_t>(off + static_cast<int16_t>(asr(sc(s[2]) - 0x40, 1))), -0x20, 0x1F);
    int f1lo = asr(sc(s[0]) * 0x69, 6) + 7;
    int f1hi = asr(sc(s[1]) * 0x69, 6) + 7;
    int x1lo = asr(f1lo, 1) + 0x74;
    int x1hi = asr(f1hi, 1) + 0x74;
    uint32_t ff1 = u(f1hi) | u(f1lo) * 0x10000u;
    b.w[1] = (u(g1 + x1hi) * 0x100u) | ff1 | (u(g1 + x1lo) * 0x1000000u);
    b.w[4] = (u(g1 - x1hi) * 0xFFFFFF00u) | (u(x1lo - g1) * 0x1000000u) | ff1;
    int g2 = clampi(static_cast<int16_t>(off + static_cast<int16_t>(asr(sc(s[5]) - 0x40, 1))), -0x20, 0x1F);
    int f2lo = asr(sc(s[3]) * 0x69, 6) + 7;
    int f2hi = asr(sc(s[4]) * 0x69, 6) + 7;
    int x2lo = asr(f2lo, 1) + 0x74;
    int x2hi = asr(f2hi, 1) + 0x74;
    uint32_t ff2 = u(f2hi) | u(f2lo) * 0x10000u;
    b.w[5] = (u(g2 + x2hi) * 0x100u) | ff2 | (u(g2 + x2lo) * 0x1000000u);
    b.w[8] = (u(g2 - x2hi) * 0xFFFFFF00u) | (u(x2lo - g2) * 0x1000000u) | ff2;
    b.flags = 0xC0;
    b.ret = -3;
}

// One morph frame of Filter_Peak_Shelf_Morph_computeFrames @ 0x813e2: (freq, shelf, peak) bytes and the
// morph offset m -> (zero X, zero F, pole X, pole F, gain term).
struct PeakFrame {
    int xz, fz, xp, fp, g;
};
PeakFrame peakFrame(int16_t m, int freq, int shelf, int peak) {
    int16_t s1 = static_cast<int16_t>(m + static_cast<int16_t>(asr(peak - 0x40, 1)));
    if (s1 < -0x20) s1 = -0x20;
    else if (s1 > 0x1F) s1 = 0x1F;
    int sh = asr(shelf, 1) - 0x20;  // (int32 at +7) >> 25 with a 0..127 byte on top
    sh = sh - asr(sh, 2);
    int f = asr(freq * 0x69, 6) + 7;
    int lo = f - sh, hi = f + sh;
    int a = sh < 0 ? -sh : sh;
    if (f - a < 2) {
        if (sh < 1) {
            hi = 2;
            lo = hi + sh * -2;
        } else {
            lo = 2;
            hi = sh * 2 + 2;
        }
    } else if (a + f > 0xDD) {
        if (sh > -1) {
            hi = 0xDD;
            lo = hi + sh * -2;
        } else {
            lo = 0xDD;
            hi = sh * 2 + 0xDD;
        }
    }
    PeakFrame r;
    r.xz = clampi(asr(lo * 0x7C, 8) + 0x89 + s1, 0, 0xFF);
    r.xp = clampi(asr(hi * 0x7C, 8) + 0x89 - s1, 0, 0xFF);
    r.fz = lo;
    r.fp = hi;
    int g = asr(freq * sh, 6);
    if (g > 0x20) g = 0x20;
    r.g = g;
    return r;
}

// s[0..5] = struct bytes +6..+11: low frame freq, shelf, peak; high frame freq, shelf, peak.
void peakShelf(Block& b, int q, const uint8_t s[6]) {
    int16_t m = static_cast<int16_t>(asr(q - 0x40, 1));
    auto sc = [](uint8_t c) { return static_cast<int>(static_cast<int8_t>(c)); };
    PeakFrame lo = peakFrame(m, sc(s[0]), s[1], sc(s[2]));
    PeakFrame hi = peakFrame(m, sc(s[3]), s[4], sc(s[5]));
    b.w[4] = kNeutral;
    b.w[5] = (u(hi.xz) << 8) | (u(lo.xz) << 24) | u(hi.fz) | (u(lo.fz) << 16);
    b.w[8] = (u(hi.xp) << 8) | (u(lo.xp) << 24) | u(hi.fp) | (u(lo.fp) << 16);
    uint32_t w = u(hi.g + 0xDFCF) | (u(lo.g + 0xDFCF) * 0x10000u);
    b.w[0] = w;
    b.w[1] = w;
    b.w[9] = kGainBase;
    b.flags = 0xC0;
    b.ret = 0;
}

// ---------------------------------------------------------------- OBXa patch (reference.rs)
// Byte-wise interpolation between table entries q >> shift and the next (lpfuzz.s lerp).
uint32_t lerp(const uint32_t* table, uint32_t q, uint32_t shift, uint32_t last) {
    uint32_t i = q >> shift;
    int32_t f = static_cast<int32_t>(q & ((1u << shift) - 1));
    uint32_t a = table[i];
    if (f == 0 || i >= last) return a;
    uint32_t bb = table[i + 1];
    uint32_t out = 0;
    for (int k = 0; k < 4; k++) {
        int sh = 24 - 8 * k;
        int32_t ab = (a >> sh) & 0xFF;
        int32_t bv = (bb >> sh) & 0xFF;
        int32_t v = ab + asr((bv - ab) * f, shift);
        out = (out << 8) | (u(v) & 0xFF);
    }
    return out;
}

// The moving drive zero (lpfuzz.s zlerp): each byte a + (t - a) * q / 127 toward 0xFFE0FFE0.
uint32_t zlerp(uint32_t word, uint32_t q) {
    uint32_t out = 0;
    const int shs[4] = {24, 16, 8, 0};
    for (int k = 0; k < 4; k++) {
        int sh = shs[k];
        int32_t a = (word >> sh) & 0xFF;
        int32_t t = (0xFFE0FFE0u >> sh) & 0xFF;
        int32_t p = (t - a) * static_cast<int32_t>(q);
        int32_t quo = p / 127;  // truncating division, as the 68020 divs
        out = (out << 8) | (u(quo + a) & 0xFF);
    }
    return out;
}

// Each frame's X raised to at least 0xC0 (fuzzob.s smth).
uint32_t smth(uint32_t w) {
    if ((w >> 24) < 0xC0) w = (w & 0x00FFFFFFu) | 0xC0000000u;
    if (((w >> 8) & 0xFF) < 0xC0) w = (w & 0xFFFF00FFu) | 0x0000C000u;
    return w;
}

// High-frame F capped at 0xDF (lpfuzz.s release fcap).
uint32_t fcap(uint32_t w) { return (w & 0xFF) > 0xDF ? ((w & 0xFFFFFF00u) | 0xDF) : w; }

void lpWords(uint32_t p, uint32_t q, uint32_t& w0, uint32_t& w4, uint32_t& w8) {
    q &= 0x7F;
    uint32_t qq = p == 6 ? (q < LP6_CAP * 4 ? q : LP6_CAP * 4) : q;
    uint32_t svf = lerp(LP_SVF, qq, 2, 31);
    if (p == 2) {
        w0 = svf;
        w4 = kNeutral;
        w8 = kNeutral;
    } else if (p == 4) {
        w0 = lerp(LP_LADA, qq, 2, 31);
        w4 = lerp(LP_LADB16, qq, 3, 15);
        w8 = kNeutral;
    } else {
        w0 = lerp(LP_LADA, qq, 2, 31);
        w4 = lerp(LP_LADB16, qq, 3, 15);
        w8 = svf;
    }
}

void obxaLp(Block& b, uint32_t p, uint32_t q) {
    q &= 0x7F;
    lpWords(p, q, b.w[0], b.w[4], b.w[8]);
    uint32_t qi = p == 6 ? q - (q >> 2) : q;
    unsigned idx = (qi >> 3) & 15;
    uint32_t goff = GOFF[idx];
    int32_t rg = RGAIN[idx];
    b.w[1] = b.w[5] = kNeutral;
    if (p == 2) {
        b.w[9] = kGainBase - (goff >> 1);
        b.ret = asr(rg, 1);
    } else if (p == 4) {
        b.w[9] = kGainBase - goff;
        b.ret = rg;
    } else {
        b.w[9] = kGainBase - goff - (goff >> 1);
        b.ret = rg * 2;
    }
    b.flags = 0x40;
}

enum ObxaKind { K_HP2, K_HP4, K_BP2, K_BP4, K_CBP };

void obxaHpBp(Block& b, ObxaKind k, uint32_t q) {
    q &= 0x7F;
    for (int i : {0, 1, 4, 5, 8}) b.w[i] = kNeutral;
    // HP / Contrary: 4.71 build 2's headroom trim, base - ((q*q*K) >> 14) * 0x00010001.
    uint32_t kk = k == K_HP2 ? 32 : (k == K_HP4 ? 48 : (k == K_CBP ? 16 : 0));
    b.w[9] = kGainBase - ((q * q * kk) >> 14) * 0x00010001u;
    int32_t ret = 0;
    switch (k) {
        case K_HP2:
            b.w[1] = HP_ZERO;
            b.w[4] = lerp(HP_SVF, q, 2, 31);
            break;
        case K_HP4:
            b.w[1] = HP_ZERO;
            b.w[5] = HP_ZERO;
            b.w[4] = lerp(HP_LADA, q, 2, 31);
            b.w[8] = lerp(HP_LADB, q, 4, 7);
            break;
        case K_BP2:
            b.w[0] = BP_W01;
            b.w[1] = BP_W01;
            b.w[5] = BP_ZERO;
            b.w[8] = lerp(BP_SVF, q, 2, 31);
            ret = -static_cast<int32_t>(q >> 3);
            break;
        case K_BP4:
            b.w[1] = BP_ZERO;
            b.w[5] = BP_ZERO;
            b.w[0] = lerp(BP_LADA, q, 2, 31);
            b.w[8] = lerp(BP_LADB, q, 3, 15);
            ret = -static_cast<int32_t>(q >> 2) + static_cast<int32_t>((q * q * BP4_BOOST) >> 14);
            break;
        case K_CBP:
            b.w[0] = lerp(CBP, q, 2, 31);
            b.w[5] = CBP_ZERO;
            break;
    }
    b.ret = ret;
    b.flags = 0xC0;
}

int32_t fuzzLevel(Block& b, uint32_t q) {
    b.w[9] = kGainBase - GOFF[q >> 4];
    return asr(static_cast<int32_t>(q >> 1) * -20, 7);
}

struct FuzzOp {
    uint8_t op;  // 1 copy, 2 smth, 3 zlerp
    int dst, src;
};

void fuzzProgram(Block& b, const FuzzOp* prog, int n, uint32_t q) {
    for (int i = 0; i < n; i++) {
        uint32_t s = b.w[prog[i].src];
        b.w[prog[i].dst] = prog[i].op == 1 ? s : (prog[i].op == 2 ? smth(s) : zlerp(s, q));
    }
}

void newFilter(Block& b, int menu, int q) {
    uint32_t qq = static_cast<uint32_t>(q) & 0x7F;
    switch (menu) {
        case 21: obxaLp(b, 2, qq); break;
        case 22: obxaLp(b, 4, qq); break;
        case 23: obxaLp(b, 6, qq); break;
        case 24: obxaHpBp(b, K_HP2, qq); break;
        case 25: obxaHpBp(b, K_HP4, qq); break;
        case 26: obxaHpBp(b, K_BP2, qq); break;
        case 27: obxaHpBp(b, K_BP4, qq); break;
        case 28: obxaHpBp(b, K_CBP, qq); break;
        case 29: {  // 4 Pole LP+Fuzz: lpfuzz.s v5 release law on the OBXa 4-pole at q/2
            obxaLp(b, 4, qq >> 1);
            uint32_t w0 = fcap(b.w[0]);
            uint32_t w4 = fcap(b.w[4]);
            b.w[0] = w0;
            b.w[4] = w4;
            b.w[1] = zlerp(w0, qq);
            b.w[5] = w4;
            b.w[8] = w4;
            b.ret = fuzzLevel(b, qq);
            b.flags = 0xC0;
            break;
        }
        case 30: {  // 2 Pole LP+Fuzz: SMTH w4 <- w0, DZER w1 <- w4
            obxaLp(b, 2, qq >> 1);
            const FuzzOp p[] = {{2, 4, 0}, {3, 1, 4}};
            fuzzProgram(b, p, 2, qq);
            b.ret = fuzzLevel(b, qq);
            b.flags = 0xC0;
            break;
        }
        case 31: {  // 2nd Ord HP+Fuzz: w8 <- w4, DZER w5 <- w8
            obxaHpBp(b, K_HP2, qq >> 1);
            const FuzzOp p[] = {{1, 8, 4}, {3, 5, 8}};
            fuzzProgram(b, p, 2, qq);
            b.ret = fuzzLevel(b, qq);
            break;
        }
        case 32: {  // 2nd Ord BP+Fuzz: DZER w5 <- w5
            obxaHpBp(b, K_BP2, qq >> 1);
            const FuzzOp p[] = {{3, 5, 5}};
            fuzzProgram(b, p, 1, qq);
            b.ret = fuzzLevel(b, qq);
            break;
        }
        case 33: {  // Contrary BP+Fuzz: w4 <- w0, DZER w1 <- w4
            obxaHpBp(b, K_CBP, qq >> 1);
            const FuzzOp p[] = {{1, 4, 0}, {3, 1, 4}};
            fuzzProgram(b, p, 2, qq);
            b.ret = fuzzLevel(b, qq);
            break;
        }
        default: break;
    }
}

std::string fmt(const char* f, int a, int bv = 0, int c = 0) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a, bv, c);
    return buf;
}

}  // namespace

const FilterDesc& filterDesc(int menu) {
    if (menu < 0 || menu >= kNumFilters) menu = 0;
    return DESCS[menu];
}

int filFreq(int v, int maxf, int mul) {
    int f = maxf;
    int n = 0xFF - v;
    while (n > 0) {
        f = mul * f;
        if (f < 0) f += 0x3FF;
        f >>= 10;
        n--;
    }
    return f;
}

Block computeFrames(int menu, int q, const uint8_t params[6], uint32_t fill) {
    Block b = blank(fill);
    q &= 0x7F;
    switch (menu) {
        case 0: lp(b, 1, q); break;
        case 1: lp(b, 2, q); break;
        case 2: lp(b, 3, q); break;
        case 3: hp(b, 1, q); break;
        case 4: hp(b, 2, q); break;
        case 5: bp(b, 1, q); break;
        case 6: bp(b, 2, q); break;
        case 7: bp(b, 3, q); break;
        case 8: sweptEq(b, 1, q); break;
        case 9: sweptEq(b, 2, q); break;
        case 10: sweptEq(b, 3, q); break;
        case 11: phaser(b, 1, q); break;
        case 12: phaser(b, 2, q); break;
        case 13: batPhaser(b, q); break;
        case 14: flanger(b, q); break;
        case 15: vocal(b, 0, q); break;
        case 16: vocal(b, 1, q); break;
        case 17: dualEq(b, 1, q, params); break;
        case 18: dualEq(b, 2, q, params); break;
        case 19: dualEq(b, 3, q, params); break;
        case 20: peakShelf(b, q, params); break;
        default:
            if (menu >= 21 && menu < kNumFilters) newFilter(b, menu, q);
            else {  // Filter_Default_computeFrames @ 0x80412
                b.w[0] = b.w[1] = b.w[4] = b.w[5] = b.w[8] = kNeutral;
                b.w[9] = kGainBase;
                b.flags = 0x40;
                b.ret = 0;
            }
            break;
    }
    return b;
}

static std::string gainDb(int v) {
    // FUN_000811e4 / Swept EQ packQ: (v * 120 >> 5) - 240 in 0.1 dB, "%s%d.%1ddB".
    int t = asr(v * 0x78, 5) - 0xF0;
    int a = t < 0 ? -t : t;
    return fmt(t < 0 ? "-%d.%1ddB" : "+%d.%1ddB", a / 10, a % 10);
}

const char* extraParamLabel(int menu, int idx) {
    if (idx < 2 || idx > 7) return "";
    if (menu >= 17 && menu <= 19) {
        switch (idx) {
            case 2: case 5: return "Low";
            case 3: case 6: return "High";
            default: return "Gain";
        }
    }
    if (menu == 20) {
        switch (idx) {
            case 2: case 5: return "Freq";
            case 3: case 6: return "Shelf";
            default: return "Peak";
        }
    }
    return "";
}

void formatParam(int menu, int idx, int value, std::string& label, std::string& text) {
    const FilterDesc& d = filterDesc(menu);
    if (idx == 0) {
        label = d.label0;
        switch (d.law) {
            case FreqLaw::Lowpass: text = fmt("%dHz", filFreq(value, 20000, 1002)); break;
            case FreqLaw::Highpass: text = fmt("%dHz", filFreq(value, 18000, 1003)); break;
            case FreqLaw::Bandpass: text = fmt("%dHz", filFreq(value, 10000, 1005)); break;
            case FreqLaw::Shelf: text = fmt("%dHz", filFreq(value, 10000, 1006)); break;
            default: text = fmt("%d", value); break;
        }
        return;
    }
    if (idx == 1) {
        label = d.label1;
        text = d.dbDisplay1 ? gainDb(value) : fmt("%d", value);
        return;
    }
    label = extraParamLabel(menu, idx);
    if (menu >= 17 && menu <= 19) {
        if (idx == 4 || idx == 7) text = gainDb(value);
        else text = fmt("%dHz", filFreq(value * 2, 10000, 1006));
    } else if (menu == 20) {
        if (idx == 4 || idx == 7) text = gainDb(value);
        else if (idx == 3 || idx == 6) text = fmt("%d", value - 0x40);
        else text = fmt("%dHz", filFreq(value * 2, 10000, 1006));
    } else {
        text = "";
    }
}

}  // namespace emuz
