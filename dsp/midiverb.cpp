#include "midiverb.hpp"

#include <cmath>
#include <cstring>

namespace midiverb {

#include "midiverb_roms.inc"

namespace {

// JavaScript ToInt32 and ToInt16 (Int16Array store) of a number.
int32_t toInt32(double x) {
    if (!std::isfinite(x)) return 0;
    double t = std::trunc(x);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return int32_t(uint32_t(m));
}
int16_t toInt16(double x) {
    if (!std::isfinite(x)) return 0;
    double t = std::trunc(x);
    double m = std::fmod(t, 65536.0);
    if (m < 0) m += 65536.0;
    return int16_t(uint16_t(m));
}
// x >> 1 on a JS number.
inline double jsShr1(double x) { return double(toInt32(x) >> 1); }

const char* const kNamesMidiverb[] = {
    ".2 Sec SMALL BRIGHT", ".2 Sec SMALL WARM", ".2 Sec MEDIUM BRIGHT",
    ".3 Sec SMALL BRIGHT", ".3 Sec SMALL WARM", ".4 Sec MEDIUM BRIGHT",
    ".4 Sec MEDIUM WARM", ".6 Sec SMALL BRIGHT", ".6 Sec MEDIUM WARM",
    ".6 Sec MEDIUM DARK", ".8 Sec SMALL BRIGHT", ".8 Sec LARGE WARM",
    "1.0 Sec SMALL WARM", "1.0 Sec MEDIUM WARM", "1.0 Sec LARGE BRIGHT",
    "1.2 Sec MEDIUM WARM", "1.2 Sec SMALL WARM", "1.2 Sec SMALL BRIGHT",
    "1.4 Sec LARGE WARM", "1.4 Sec LARGE DARK", "1.4 Sec MEDIUM WARM",
    "1.6 Sec SMALL DARK", "1.6 Sec LARGE BRIGHT", "1.6 Sec MEDIUM BRIGHT",
    "1.8 Sec LARGE DARK", "1.8 Sec LARGE BRIGHT", "1.8 Sec MEDIUM WARM",
    "2.0 Sec LARGE BRIGHT", "2.0 Sec MEDIUM WARM", "2.0 Sec LARGE WARM",
    "2.5 Sec MEDIUM WARM", "2.5 Sec LARGE BRIGHT", "2.5 Sec MEDIUM DARK",
    "2.8 Sec SMALL BRIGHT", "2.8 Sec MEDIUM BRIGHT", "3.0 Sec LARGE BRIGHT",
    "3.0 Sec LARGE WARM", "3.0 Sec MEDIUM DARK", "3.5 Sec LARGE BRIGHT",
    "3.5 Sec MEDIUM WARM", "4.0 Sec LARGE DARK", "4.0 Sec MEDIUM BRIGHT",
    "5 Sec LARGE WARM", "8 Sec LARGE BRIGHT", "8 Sec LARGE WARM",
    "10 Sec LARGE BRIGHT", "10 Sec LARGE WARM", "16 Sec LARGE DARK",
    "18 Sec EX. LARGE BRIGHT", "20 Sec EX. LARGE DARK", "Gated 100 MS",
    "Gated 150 MS", "Gated 200 MS", "Gated 250 MS", "Gated 300 MS",
    "Gated 350 MS", "Gated 400 MS", "Gated 500 MS", "Gated 600 MS",
    "REVERSE - 300 MS", "REVERSE - 400 MS", "REVERSE - 500 MS",
    "REVERSE - 600 MS", "DEFEAT"};

const char* const kNamesMidifex[] = {
    "ECHO LONG FLAT AMBI", "ECHO LONG FLAT THICK", "ECHO LONG HPF",
    "ECHO LONG HPF WIDE", "ECHO LONG BPF AMBI", "ECHO LONG LPF WIDE",
    "ECHO MED FLAT AMBI", "ECHO MED FLAT WIDE", "ECHO MED HPF AMBI",
    "ECHO MED BPF AMBI", "ECHO MED LPF AMBI", "ECHO MED LPF WIDE",
    "ECHO MED FLAT THICK", "ECHO SHORT FLAT", "ECHO SHORT LPF AMBI",
    "ECHO SHORT HPF AMBI", "ECHO SHORT BPF AMBI", "ECHO SHORT FLAT",
    "ECHO SHORT FLAT WIDE", "ECHO XSHORT FLAT", "ECHO XSHORT BPF WIDE",
    "2TAP MED FLAT AMBI", "2TAP MED HPF AMBI", "2TAP MED BPF AMBI",
    "2TAP MED FLAT THICK", "2TAP SHORT FLAT WIDE", "2TAP SHORT HPF PAN",
    "2TAP SHORT BPF AMBI", "2TAP SHORT LPF AMBI", "2TAP XSHORT FLAT WIDE",
    "3TAP MED FLAT PAN", "3TAP SHORT FLAT PAN", "3TAP SHORT LPF AMBI",
    "3TAP SHORT BPF AMBI", "3TAP SHORT HPF AMBI", "3TAP XSHORT FLAT AMBI",
    "REGEN MED FLAT", "REGEN MED HPF AMBI", "REGEN MED BPF AMBI",
    "REGEN MED LPF AMBI", "REGEN SHORT FLAT", "REGEN XSHORT FLAT",
    "SLAP1", "SLAP2", "SLAP3", "SLAP4", "SLAP5",
    "REVERB SHORT GATE", "REVERB MEDIUM WARM", "REVERB MEDIUM BLOOM",
    "REVERB MEDIUM PAN", "REVERB LONG HPF", "REVERB REVERSE",
    "REVERB REVERSE REGEN", "MULTITAP PAN", "MULTITAP REVERB",
    "MULTITAP REVERSE PAN", "THICKENER", "THICKENER DENSE",
    "STEREOGEN AMBIENT", "STEREOGEN THICK", "STEREOGEN WIDE",
    "STEREOGEN XWIDE", "DEFEAT"};

const char* const kNamesMidiverb2[] = {
    "Defeat", "Small Bright .1 Sec", "Small Bright .2 Sec", "Small Bright .3 Sec",
    "Medium Warm 1.1 Sec", "Medium Bright .6 Sec", "Large Bright 1.2 Sec",
    "Large Dark 1.0 Sec", "Medium Dark .6 Sec", "Medium Dark .5 Sec",
    "Medium Bright .6 Sec", "Medium Bright .8 Sec", "Large Warm 1.0 Sec",
    "Large Warm 1.1 Sec", "Medium Dark 1.0 Sec", "Medium Bright 1.1 Sec",
    "Medium Bright 1.15 Sec", "Large Bright 1.6 Sec", "Large Dark 1.7 Sec",
    "Medium Bright 1.65 Sec", "Medium Bright 1.9 Sec", "Large Warm 2.2 Sec",
    "Large Warm 1.75 Sec", "Large Bright 1.45 Sec", "Large Dark 2.2 Sec",
    "Large Warm 2.3 Sec", "Large Bright 2.4 Sec", "Large Bright 2.5 Sec",
    "Xlarge Warm 5.0 Sec", "Xlarge Warm 15.0 Sec", "Slow Gate 100 Msec",
    "Slow Gate 200 Msec", "Slow Gate 250 Msec", "Slow Gate 400 Msec",
    "Slow Gate 450 Msec", "Fast Gate 150 Msec", "Fast Gate 75 Msec",
    "Fast Gate 200 Msec", "Fast Gate 100 Msec", "Fast Gate 175 Msec",
    "Reverse 400 Msec", "Reverse 450 Msec", "Reverse 250 Msec",
    "Reverse 200 Msec", "Reverse 150 Msec", "Bloom 1 8 Sec",
    "Reverse 500 Msec", "Reverse Regen. 2 Sec", "Reverse Regen. 8 Sec",
    "Bloom 2 7 Sec", "Triggered Flange", "Flange Pan 1", "Flange Pan 2",
    "Triggered Flange 2", "Flange Pan 3", "Flange Pan 4", "Flange Pan 5",
    "Triggered Flange 3", "Flange Pan 6", "Flange Pan 7", "Light Chorus 1",
    "Light Chorus 2", "Medium Chorus 1", "Medium Chorus 2", "Medium Chorus 3",
    "Deep Chorus 1", "Deep Chorus 2", "Deep Chorus 3", "Fast Chorus 1",
    "Fast Chorus 2", "35 Msec", "55 Msec", "75 Msec", "115 Msec", "140 Msec",
    "155 Msec", "160 Msec", "170 Msec", "175 Msec", "180 Msec", "185 Msec",
    "195 Msec", "205 Msec", "210 Msec", "220 Msec", "240 Msec", "250 Msec",
    "275 Msec", "375 Msec", "460 Msec", "2 Tap Ambient", "3 Tap Pan",
    "Multitap", "Multitap Reverse Pan", "Thickener/frozen Flange",
    "Stereo Generation", "Stereo Generation Wide", "Regenerated Delay 2 Sec",
    "Regenerated Delay 3 Sec", "Regenerated Delay 4 Sec"};

const RomInfo kRoms[3] = {
    {"MIDIVerb", kRomMidiverb, sizeof kRomMidiverb, 1, 64, 0, 2, false, 0, kNamesMidiverb},
    {"MIDIFex", kRomMidifex, sizeof kRomMidifex, 1, 64, 0, 2, false, 0, kNamesMidifex},
    {"MIDIVerb 2", kRomMidiverb2, sizeof kRomMidiverb2, 0, 99, 0x1c00, 1, true, 0x1b00, kNamesMidiverb2},
};

// LFO_PATCHES (MIDIVerb 2 programs 50..69).
const LfoPatch kLfoPatches[20] = {
    {false, 0x3fff, true, 0x00, 0x00, 0x03, 0x03, 0xc0},
    {false, 0x3fff, false, 0x00, 0x00, 0x03, 0x05, 0x80},
    {false, 0x3fff, false, 0x00, 0x00, 0x05, 0x05, 0xc0},
    {false, 0x7fff, true, 0x00, 0x00, 0x06, 0x07, 0x80},
    {false, 0x7fff, false, 0x00, 0x00, 0x05, 0x05, 0xc0},
    {false, 0x7fff, false, 0x00, 0x00, 0x0c, 0x0c, 0x80},
    {false, 0x7fff, false, 0x00, 0x00, 0x11, 0x11, 0xc0},
    {false, 0xffff, true, 0x00, 0x00, 0x13, 0x17, 0x80},
    {false, 0xffff, false, 0x00, 0x00, 0x0d, 0x0e, 0xc0},
    {false, 0xffff, false, 0x00, 0x00, 0x17, 0x11, 0x80},
    {true, 0x7eff, false, 0x01, 0x03, 0x27, 0x1b, 0x80},
    {true, 0x3eff, false, 0x02, 0x03, 0x2e, 0x3d, 0x80},
    {true, 0x7eff, false, 0x02, 0x01, 0x2d, 0x3a, 0x80},
    {true, 0x7eff, false, 0x03, 0x02, 0x40, 0x45, 0x80},
    {true, 0x3eff, false, 0x02, 0x03, 0x4f, 0x79, 0x80},
    {true, 0x3eff, false, 0x02, 0x01, 0x94, 0x7a, 0x80},
    {true, 0x7eff, false, 0x02, 0x01, 0x4f, 0x41, 0x80},
    {true, 0x3eff, false, 0x01, 0x02, 0x70, 0x8e, 0x80},
    {true, 0x1eff, false, 0x01, 0x01, 0xea * 4, 0xd9 * 4, 0x80},
    {true, 0x1eff, false, 0x01, 0x01, 0xff * 4, 0xff * 4, 0x80},
};

}  // namespace

int romCount() { return 3; }
const RomInfo& rom(int index) { return kRoms[index < 0 ? 0 : (index > 2 ? 2 : index)]; }
const LfoPatch* lfoPatch(int program) { return (program >= 50 && program <= 69) ? &kLfoPatches[program - 50] : nullptr; }

// ---------------------------------------------------------------- Lfo
void Lfo::init(bool sine, int32_t rate, int32_t max) {
    sine_ = sine;
    rate_ = rate;
    max_ = max;
    reset();
}

void Lfo::reset() {
    if (!sine_) {
        value_ = 0;
        dir_ = 1;
    } else {
        x_ = 0;
        y_ = max_ & 0xff00;
    }
}

int32_t Lfo::update() {
    if (!sine_) {
        if (dir_) {
            value_ += rate_;
            if (value_ > max_) {
                value_ = max_;
                dir_ = 0;
            }
        } else {
            value_ -= rate_;
            if (value_ < 0) {
                value_ = 0;
                dir_ = 1;
            }
        }
        return value_ & 0xffff;
    }
    const int32_t clampPos = max_, clampNeg = -max_;
    y_ += (x_ * rate_) >> 16;
    if (y_ > clampPos) y_ = clampPos;
    if (y_ < clampNeg) y_ = clampNeg;
    x_ -= (y_ * rate_) >> 16;
    if (x_ > clampPos) x_ = clampPos;
    if (x_ < clampNeg) x_ = clampNeg;
    return (x_ ^ 0x8000) & 0xffff;
}

// ---------------------------------------------------------------- Filterx2
double Filterx2::run(double x) {
    const double GAIN = 1.481376007e+01;
    xv0 = xv1; xv1 = xv2; xv2 = xv3; xv3 = xv4;
    xv4 = x / GAIN;
    yv0 = yv1; yv1 = yv2; yv2 = yv3; yv3 = yv4;
    yv4 = (xv0 + xv4) + 4 * (xv1 + xv3) + 6 * xv2
        + (-0.0206514245 * yv0) + (0.0842337122 * yv1)
        + (-0.5343006371 * yv2) + (0.3906414532 * yv3);
    return yv4;
}

// ---------------------------------------------------------------- Machine
void Machine::loadRom(const RomInfo* r) {
    rom_ = r;
    memoryShift_ = r->memoryShift;
}

void Machine::loadProgram(int programNum) {
    if (!rom_) return;
    const int programIndex = programNum - rom_->firstProgram;
    const int offset = rom_->bytecodeOffset + programIndex * kProgramByteLength;
    for (int i = 0; i < kProgramByteLength; i++) {
        size_t a = size_t(offset + i);
        program_[i] = a < rom_->size ? rom_->data[a] : 0;
    }
    if (rom_->hasLfo && rom_->interpolationPatchTableOffset) {
        for (int i = 0; i < 256; i++) interpolationPatchTable_[i] = rom_->data[rom_->interpolationPatchTableOffset + i];
    }
    hasActiveLfo_ = false;
    const LfoPatch* p = rom_->hasLfo ? lfoPatch(programNum) : nullptr;
    if (p) {
        lfo1.init(p->sine, p->rate1, p->max);
        lfo2.init(p->sine, p->rate2, p->max);
        hasLfoObjects_ = true;
        lfoPatch_ = p;
        hasActiveLfo_ = true;
    }
}

void Machine::resetMachine() {
    std::memset(dram_, 0, sizeof dram_);
    acc_ = 0;
    address_ = 0;
    if (hasLfoObjects_) {
        lfo1.reset();
        lfo2.reset();
    }
}

void Machine::patchMachine(int32_t lfo1Value, int32_t lfo2Value) {
    if (!lfoPatch_) return;
    const LfoPatch& patch = *lfoPatch_;
    uint8_t* program = program_;
    const uint8_t* interpTable = interpolationPatchTable_;
    {
        const int32_t memoryShift = lfo1Value | (patch.top1 << 16);
        const int32_t patchOffset = memoryShift & 0xf0;
        const int32_t sub = ((interpTable[patchOffset + 1] << 8) | interpTable[patchOffset]) - (memoryShift >> 8);
        const int32_t add = ((interpTable[patchOffset + 7] << 8) | interpTable[patchOffset + 6]) + (memoryShift >> 8);
        program[0x05] = uint8_t(sub & 0xff);
        program[0x06] = uint8_t(((sub >> 8) & 0x3f) | 0x40);
        program[0x59] = interpTable[patchOffset + 2];
        program[0x5a] = interpTable[patchOffset + 3];
        program[0x5b] = interpTable[patchOffset + 4];
        program[0x5c] = interpTable[patchOffset + 5];
        program[0x5d] = uint8_t(add & 0xff);
        program[0x5e] = uint8_t(((add >> 8) & 0x3f) | patch.nextInstrOpcode);
    }
    {
        const int32_t memoryShift = lfo2Value | (patch.top2 << 16);
        const int32_t patchOffset = 8 + (memoryShift & 0xf0);
        const int32_t sub = ((interpTable[patchOffset + 1] << 8) | interpTable[patchOffset]) - (memoryShift >> 8);
        const int32_t add = ((interpTable[patchOffset + 7] << 8) | interpTable[patchOffset + 6]) + (memoryShift >> 8);
        program[0x5f] = uint8_t(sub & 0xff);
        program[0x60] = uint8_t(((sub >> 8) & 0x3f) | 0x40);
        program[0xb3] = interpTable[patchOffset + 2];
        program[0xb4] = interpTable[patchOffset + 3];
        program[0xb5] = interpTable[patchOffset + 4];
        program[0xb6] = interpTable[patchOffset + 5];
        program[0xb7] = uint8_t(add & 0xff);
        program[0xb8] = uint8_t(((add >> 8) & 0x3f) | patch.nextInstrOpcode);
    }
}

void Machine::runMachineTick(double input, double out[2]) {
    double acc = acc_;
    int32_t address = address_;
    out[0] = 0;
    out[1] = 0;
    const int memShift = memoryShift_;
    const uint8_t* program = program_;

    for (int pc = 0; pc < 128; pc++) {
        const int operation = program[(2 * pc - memShift - 1) & 0xff] >> 6;
        const int offsetLo = program[(2 * pc - memShift + 0) & 0xff];
        const int offsetHi = program[(2 * pc - memShift + 1) & 0xff] & 0x3f;
        const int offset = (offsetHi << 8) | offsetLo;

        double newAcc = acc;
        double data = 0;
        int sgn;
        switch (operation) {
            case 0:  // sumhlf
                data = dram_[address];
                sgn = data < 0 ? 1 : 0;
                newAcc = newAcc + jsShr1(data) + sgn;
                break;
            case 1:  // ldhlf
                data = dram_[address];
                sgn = data < 0 ? 1 : 0;
                newAcc = jsShr1(data) + sgn;
                break;
            case 2:  // strpos
                data = newAcc;
                sgn = data < 0 ? 1 : 0;
                dram_[address] = toInt16(data);
                newAcc = newAcc + jsShr1(data) + sgn;
                break;
            default:  // strneg
                data = double(~toInt32(newAcc));
                sgn = data < 0 ? 1 : 0;
                dram_[address] = toInt16(data);
                newAcc = jsShr1(data) + sgn;
                break;
        }

        if (pc == 0x60) {
            out[0] = data;
        } else if (pc == 0x70) {
            out[1] = data;
        } else if (pc == 0) {
            dram_[address] = toInt16(input);
        } else {
            acc = newAcc;
        }

        address = (address + offset) % kDramLength;
    }
    acc_ = acc;
    address_ = address;
}

static double clip(double input) {
    if (input > 32767) return 32767;
    if (input < -32768) return -32768;
    return input;
}

void Machine::runEffect(double leftIn, double rightIn, double feedbackAmount, double out[2]) {
    const double monoSample = double(toInt32(leftIn + rightIn + lastOutput_ * feedbackAmount) >> 5);
    double s[2];
    runMachineTick(monoSample, s);
    const double leftOut = clip(s[0] * 16);
    const double rightOut = clip(s[1] * 16);
    lastOutput_ = (leftOut + rightOut) / 2;
    out[0] = leftOut;
    out[1] = rightOut;
}

// ---------------------------------------------------------------- Emulator
Emulator::Emulator() { selectRom(2); selectProgram(1); }

void Emulator::selectRom(int index) {
    romIndex_ = index < 0 ? 0 : (index >= romCount() ? romCount() - 1 : index);
    const RomInfo& r = rom(romIndex_);
    program_ = r.firstProgram;
    machine_.loadRom(&r);
    machine_.loadProgram(program_);
    machine_.resetMachine();
}

void Emulator::selectProgram(int programNum) {
    const RomInfo& r = rom(romIndex_);
    if (programNum < r.firstProgram) programNum = r.firstProgram;
    if (programNum > r.lastProgram) programNum = r.lastProgram;
    program_ = programNum;
    machine_.loadProgram(program_);
    machine_.resetMachine();
}

void Emulator::process(double left, double right, double& outLeft, double& outRight) {
    const double leftDs = down0_.run(left);
    const double rightDs = down1_.run(right);
    if (sample_ % 2 == 0) {
        double out[2];
        machine_.runEffect(double(toInt32(leftDs * 32767.0)), double(toInt32(rightDs * 32767.0)), feedbackAmount, out);
        lastLeft_ = out[0] / 32767.0;
        lastRight_ = out[1] / 32767.0;
        if (machine_.hasActiveLfo() && lfoTickCounter_ % 8 == 0) {
            const int32_t lfo1Value = machine_.lfo1.update();
            const int32_t lfo2Value = machine_.lfo2.update();
            machine_.patchMachine(lfo1Value, lfo2Value);
        }
        lfoTickCounter_++;
    }
    sample_++;
    const double leftUs = up0_.run(lastLeft_);
    const double rightUs = up1_.run(lastRight_);
    outLeft = (1 - dryWetMix) * left + dryWetMix * leftUs;
    outRight = (1 - dryWetMix) * right + dryWetMix * rightUs;
}

}  // namespace midiverb
