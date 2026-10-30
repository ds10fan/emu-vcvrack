// Port of the JavaScript Alesis MIDIverb / MIDIFex / MIDIVerb 2 emulator of
// http://ibawizard.net/midiverb/ (the page's inline script): the ROM program interpreter
// (128 instructions per sample over a 16K-word DRAM), the MIDIVerb 2 LFO program patching, the
// feedback path, and the page's resampling: a 4th-order IIR before and after, the machine run on
// every second host sample with its output held in between. Translated line for line, including
// JavaScript's number semantics (the accumulator is a double, `>>` and `~` convert to int32, DRAM
// stores wrap to 16 bits), so that it reproduces the JS bit for bit (test/midiverb_test.cpp).
// No Rack dependency.
#pragma once
#include <cstddef>
#include <cstdint>

namespace midiverb {

struct RomInfo {
    const char* name;
    const uint8_t* data;
    size_t size;
    int firstProgram, lastProgram;
    int bytecodeOffset;
    int memoryShift;
    bool hasLfo;
    int interpolationPatchTableOffset;
    const char* const* effectNames;
    int programCount() const { return lastProgram - firstProgram + 1; }
};

int romCount();
const RomInfo& rom(int index);  // 0 MIDIVerb, 1 MIDIFex, 2 MIDIVerb 2 (the JS's ROM_TYPES order)

struct LfoPatch {
    bool sine;
    int32_t max;
    bool retrigger;  // in the JS table, unused there too
    int32_t top1, top2, rate1, rate2;
    int32_t nextInstrOpcode;
};
// LFO_PATCHES of the JS: MIDIVerb 2 programs 50..69, nullptr for the others.
const LfoPatch* lfoPatch(int program);

class Lfo {
public:
    void init(bool sine, int32_t rate, int32_t max);
    void reset();
    int32_t update();

private:
    bool sine_ = false;
    int32_t rate_ = 0, max_ = 0;
    int32_t value_ = 0, dir_ = 1, x_ = 0, y_ = 0;
};

// The JS's Filterx2: a 4th-order IIR, used for both down- and upsampling.
class Filterx2 {
public:
    double run(double x);

private:
    double xv0 = 0, xv1 = 0, xv2 = 0, xv3 = 0, xv4 = 0;
    double yv0 = 0, yv1 = 0, yv2 = 0, yv3 = 0, yv4 = 0;
};

// The JS's Machine.
class Machine {
public:
    static constexpr int kProgramByteLength = 256;
    static constexpr int kDramLength = 16 * 1024;

    void loadRom(const RomInfo* r);
    void loadProgram(int programNum);
    void resetMachine();
    void patchMachine(int32_t lfo1Value, int32_t lfo2Value);
    void runMachineTick(double input, double out[2]);
    void runEffect(double leftIn, double rightIn, double feedbackAmount, double out[2]);
    bool hasActiveLfo() const { return hasActiveLfo_; }
    Lfo lfo1, lfo2;

private:
    const RomInfo* rom_ = nullptr;
    uint8_t program_[kProgramByteLength] = {};
    uint8_t interpolationPatchTable_[256] = {};
    int16_t dram_[kDramLength] = {};
    double acc_ = 0;
    int32_t address_ = 0;
    double lastOutput_ = 0;
    int memoryShift_ = 2;
    const LfoPatch* lfoPatch_ = nullptr;
    bool hasLfoObjects_ = false;
    bool hasActiveLfo_ = false;
};

// The page around the machine: device and program selection, dry/wet mix, feedback, and the
// per-host-sample loop of the ScriptProcessor callback. Inputs and outputs are floats in -1..1
// as the Web Audio buffers hold them.
class Emulator {
public:
    Emulator();
    // Device select: the program goes to the ROM's first program; machine reset.
    void selectRom(int index);
    // Program select (a program number as the ROM numbers them); machine reset.
    void selectProgram(int programNum);
    int romIndex() const { return romIndex_; }
    int program() const { return program_; }
    double dryWetMix = 0.5;
    double feedbackAmount = 0.0;
    void process(double left, double right, double& outLeft, double& outRight);

private:
    Machine machine_;
    Filterx2 down0_, down1_, up0_, up1_;
    double lastLeft_ = 0, lastRight_ = 0;
    uint64_t sample_ = 0;
    uint64_t lfoTickCounter_ = 0;
    int romIndex_ = 2;
    int program_ = 1;
};

}  // namespace midiverb
