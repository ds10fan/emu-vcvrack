// fscript: a small per-sample DSP language for writing filters in the module's editor.
//
// Source is compiled to bytecode for a stack machine over a flat per-voice memory of doubles
// (builtins, state variables, buffers). Every variable is per-voice state: it keeps its value
// from one sample to the next and starts at zero. See README.md (Filter Lab) for the language.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fscript {

// Builtin slots at the start of a voice's memory. Inputs are written by the host before each
// sample; out..out4 are read after it.
enum Builtin {
    B_IN,
    B_IN2,
    B_IN3,
    B_IN4,
    B_P1,
    B_P2,
    B_P3,
    B_P4,
    B_CV1,
    B_CV2,
    B_CV3,
    B_CV4,
    B_SR,
    B_DT,
    B_CH,
    B_OUT,
    B_OUT2,
    B_OUT3,
    B_OUT4,
    B_COUNT
};

struct Ins {
    uint16_t op;
    int32_t a;
};

struct Var {
    std::string name;
    int offset;   // into voice memory
    int size;     // 1 for a scalar
    bool buf;     // declared with buf
    bool fresh;   // never carried over on a live edit (assigned in init, table buffers, temporaries)
};

struct BufInfo {
    int offset;
    int size;
    std::vector<double> init;  // table buffers: initial contents (size entries)
};

struct Program {
    std::vector<Ins> code;      // run every sample
    std::vector<Ins> initCode;  // run on voice reset
    std::vector<double> consts;
    std::vector<BufInfo> bufs;
    std::vector<Var> vars;
    int memSize = B_COUNT;

    // header directives
    std::string name;
    std::string paramLabel[4];
    std::string inLabel[4];   // #in (or #in1), #in2, #in3, #in4
    std::string outLabel[4];  // #out (or #out1), #out2, #out3, #out4
};

struct Error {
    int line = 0;
    int col = 0;
    std::string msg;
};

// Compiles src into prog. On failure returns false and fills err (1-based line and column).
bool compile(const std::string& src, Program& prog, Error& err);

// Upper bound on loop iterations per run; a run that exceeds it stops and reports failure.
const int kLoopBudget = 1 << 16;

struct Voice {
    std::vector<double> mem;
    uint32_t rng = 0x12345678u;
};

// Allocates the voice's memory for prog (zeroed); call reset before running.
void prepare(const Program& prog, Voice& v);
// Zeroes the memory, loads table buffers, sets sr, dt, ch and runs the init code.
// The host may set inputs (in, p1..) before the call; they are kept.
bool reset(const Program& prog, Voice& v, double sampleRate, int channel);
// Runs one sample. Returns false if the loop budget ran out.
bool run(const Program& prog, Voice& v);
// Copies the state variables the old program shares with the new one (same name and size,
// not fresh in the new program) from old voice to new voice.
void transfer(const Program& from, const Voice& vf, const Program& to, Voice& vt);

// Names of the builtin functions, for the editor's reference.
std::string reference();

}  // namespace fscript
