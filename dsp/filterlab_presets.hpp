// Factory filter models of Filter Lab, written in fscript.
#pragma once

namespace filterlab {

struct Preset {
    const char* name;
    const char* code;
};

extern const Preset kPresets[];
extern const int kPresetCount;

}  // namespace filterlab
