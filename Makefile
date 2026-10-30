# Rack SDK location; unpack Rack-SDK-<version>-<os>-<cpu>.zip into sdk/ or point RACK_DIR elsewhere.
RACK_DIR ?= sdk/Rack-SDK

FLAGS += -Idsp
SOURCES += $(wildcard src/*.cpp) dsp/hchip.cpp dsp/emu_filters.cpp dsp/gchip.cpp dsp/wav.cpp dsp/midiverb.cpp
DISTRIBUTABLES += res

include $(RACK_DIR)/plugin.mk

# The MidiVerb port reproduces the JavaScript's double arithmetic bit for bit only without the
# SDK's -funsafe-math-optimizations and without contracted multiply-adds.
build/dsp/midiverb.cpp.o: CXXFLAGS += -fno-unsafe-math-optimizations -ffp-contract=off

# The DSP tests (no Rack needed)
test:
	$(MAKE) -C test check

.PHONY: test
